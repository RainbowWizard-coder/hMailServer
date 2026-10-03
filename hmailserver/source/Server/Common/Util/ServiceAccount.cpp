// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The account the server runs as, and the grant a less privileged one needs.
// See ServiceAccount.h and docs/ServiceAccount.md.

#include "StdAfx.h"
#include "ServiceAccount.h"

#ifdef HM_PLATFORM_POSIX

#include <cerrno>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#else

#include <winsvc.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <openssl/evp.h>

#include "FileUtilities.h"
#include "GUIDCreator.h"
#include "ProtectedFile.h"
#include "RestApiServer.h"
#include "ServiceManager.h"
#include "Unicode.h"
#include "UpdateDownloader.h"
#include "../Application/IniFileSettings.h"
#include "../Application/Logger.h"
#include "../AntiSpam/DKIM/DkimRotation.h"
#include "../Persistence/SchemaUpgrader.h"
#include "../SQL/DALConnection.h"
#include "../SQL/DALConnectionFactory.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseSettings.h"
#include "../SQL/SQLCommand.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

// SHGetKnownFolderPath and CoTaskMemFree, for the folders the grant guards.
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
#ifdef HM_PLATFORM_POSIX

   String
   ServiceAccount::ProcessAccountName()
   {
      // getpwuid_r, into a buffer of this call's own: getpwuid's is one for the
      // whole process, and the setup checklist, auto-ban and the updater ask for
      // the name from threads of their own. A buffer too small for the entry is
      // doubled, up to a megabyte.
      const long suggested = ::sysconf(_SC_GETPW_R_SIZE_MAX);
      std::vector<char> buffer(suggested > 0 ? (size_t) suggested : 16384);
      for (;;)
      {
         struct passwd entry = {};
         struct passwd *found = nullptr;
         const int error = ::getpwuid_r(::geteuid(), &entry, buffer.data(), buffer.size(), &found);
         if (error == ERANGE && buffer.size() < 1024 * 1024)
         {
            buffer.resize(buffer.size() * 2);
            continue;
         }
         if (error != 0 || found == nullptr || found->pw_name == nullptr)
            return String();
         return String(found->pw_name);
      }
   }

   bool
   ServiceAccount::ProcessIsMostPrivileged()
   {
      return ::geteuid() == 0;
   }

   bool
   ServiceAccount::ProcessIsAdministrator()
   {
      return ::geteuid() == 0;
   }

#else

   namespace
   {
      // The user SID of this process's token, in buffer; nullptr when the token
      // cannot be read.
      PSID ProcessUserSid_(std::vector<BYTE> &buffer)
      {
         HANDLE token = nullptr;
         if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return nullptr;

         DWORD needed = 0;
         GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
         PSID sid = nullptr;
         if (needed > 0)
         {
            buffer.resize(needed);
            if (GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed))
               sid = ((TOKEN_USER *) buffer.data())->User.Sid;
         }

         CloseHandle(token);
         return sid;
      }

      String SidText_(PSID sid)
      {
         String text;
         LPWSTR converted = nullptr;
         if (sid != nullptr && ConvertSidToStringSidW(sid, &converted))
         {
            text = converted;
            LocalFree(converted);
         }
         return text;
      }

      // One line of the verb's report, to the handles the caller gave or the
      // console it has (hMailServer.cpp attaches them), in UTF-8.
      void SayLine_(bool error, const String &line)
      {
         AnsiString text;
         Unicode::WideToMultiByte(line, text);
         text += "\r\n";

         HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
         if (handle && handle != INVALID_HANDLE_VALUE)
         {
            DWORD written = 0;
            WriteFile(handle, text.c_str(), (DWORD) text.GetLength(), &written, nullptr);
         }
      }

      // A path as Windows resolves it, without a trailing separator, for
      // comparing one with another. The path itself when it cannot be resolved.
      String Normalised_(const String &path)
      {
         String value = path;
         value.Trim();
         if (value.IsEmpty())
            return value;

         wchar_t full[MAX_PATH * 2];
         DWORD length = GetFullPathNameW(value.c_str(), MAX_PATH * 2, full, nullptr);
         if (length > 0 && length < MAX_PATH * 2)
            value = full;

         while (value.GetLength() > 3 && (value.Right(1) == _T("\\") || value.Right(1) == _T("/")))
            value = value.Left(value.GetLength() - 1);
         return value;
      }

      // The path with every component in its long form, when it exists: an 8.3
      // name (PROGRA~1) is another spelling of the same folder, and the
      // refusals compare spellings.
      String LongPath_(const String &path)
      {
         std::vector<wchar_t> buffer(32768);
         const DWORD length = GetLongPathNameW(path.c_str(), buffer.data(), (DWORD) buffer.size());
         if (length == 0 || length >= buffer.size())
            return path;
         return String(buffer.data());
      }

      // Full, in its long form, without a trailing separator.
      String Resolved_(const String &path)
      {
         return Normalised_(LongPath_(Normalised_(path)));
      }

      bool SamePath_(const String &first, const String &second)
      {
         return Normalised_(first).CompareNoCase(Normalised_(second)) == 0;
      }

      // Whether path is directory or anything below it.
      bool IsUnder_(const String &path, const String &directory)
      {
         const String inner = Normalised_(path);
         const String outer = Normalised_(directory);
         if (inner.IsEmpty() || outer.IsEmpty())
            return false;
         if (inner.CompareNoCase(outer) == 0)
            return true;
         if (inner.GetLength() <= outer.GetLength())
            return false;
         if (inner.Left(outer.GetLength()).CompareNoCase(outer) != 0)
            return false;
         const String next = inner.Mid(outer.GetLength(), 1);
         return next == _T("\\") || next == _T("/") || outer.Right(1) == _T("\\");
      }

      // Whether text is a full path on a drive, "X:\...": not relative, not a
      // share, not a device path (\\?\, \\.\), and with no colon past the
      // drive's, which would name an alternate data stream.
      bool IsDrivePath_(const String &text)
      {
         String value = text;
         value.Trim();
         if (value.GetLength() < 3)
            return false;

         const wchar_t drive = value[0];
         if (!((drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z')))
            return false;
         if (value[1] != L':' || (value[2] != L'\\' && value[2] != L'/'))
            return false;
         return value.Find(_T(":"), 2) < 0;
      }

      // The folder a path is in; empty for a drive's root.
      String FolderOf_(const String &path)
      {
         const int separator = path.ReverseFind(_T('\\'));
         if (separator <= 2)
            return separator == 2 ? path.Left(3) : String();
         return path.Left(separator);
      }

      // A known folder of this machine - Program Files, the user profiles -
      // resolved; empty when Windows has none.
      String KnownFolder_(REFKNOWNFOLDERID id)
      {
         PWSTR found = nullptr;
         String path;
         if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &found)) && found != nullptr)
            path = found;
         if (found != nullptr)
            CoTaskMemFree(found);
         return path.IsEmpty() ? path : Resolved_(path);
      }

      // The descriptor and DACL of a file or folder, freed with it.
      struct Permissions_
      {
         PSECURITY_DESCRIPTOR descriptor;
         PACL dacl;

         Permissions_() : descriptor(nullptr), dacl(nullptr) {}
         // Releases what it holds when it goes: a copy would release it twice.
         Permissions_(const Permissions_ &) = delete;
         Permissions_ &operator=(const Permissions_ &) = delete;
         ~Permissions_()
         {
            if (descriptor)
               LocalFree(descriptor);
         }
      };

      bool ReadPermissions_(const String &path, Permissions_ &permissions, String &error)
      {
         const DWORD result = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &permissions.dacl, nullptr, &permissions.descriptor);
         if (result != ERROR_SUCCESS)
         {
            error = Formatter::Format(_T("its permissions could not be read (Windows error {0})"), (int) result);
            return false;
         }
         return true;
      }

      bool IsProtected_(const Permissions_ &permissions)
      {
         SECURITY_DESCRIPTOR_CONTROL control = 0;
         DWORD revision = 0;
         if (!permissions.descriptor || !GetSecurityDescriptorControl(permissions.descriptor, &control, &revision))
            return false;
         return (control & SE_DACL_PROTECTED) != 0;
      }

      // A mask with any generic right spelled out as the file rights it means,
      // so that an administrator's GENERIC_ALL is read as full control.
      DWORD FileRights_(DWORD mask)
      {
         GENERIC_MAPPING mapping = { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
         MapGenericMask(&mask, &mapping);
         return mask & ~(GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL);
      }

      // Whether an allow entry for sid - one that applies here and not only to
      // what is below - covers rights, and on a folder whose grant is handed
      // down, reaches both the folders and the files below it without stopping
      // at the first level.
      //
      // Windows reads explicit entries before inherited ones, and a deny before
      // an allow within each. So an explicit deny for sid (explicitDeny) takes
      // the rights away from any allow, and nothing this grant adds outranks it;
      // an inherited deny (inheritedDeny) takes them from an inherited allow but
      // not from an explicit one - so rights that only an inherited allow gives,
      // with an inherited deny against them, are not covered, and the grant's own
      // explicit entry, which comes before that deny, is what makes them so.
      bool Covers_(PACL dacl, PSID sid, DWORD rights, bool toEverythingBelow, bool &explicitDeny, bool &inheritedDeny)
      {
         explicitDeny = false;
         inheritedDeny = false;

         // No DACL at all is everybody's full control.
         if (dacl == nullptr)
            return true;

         ACL_SIZE_INFORMATION size;
         if (!GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation))
            return false;

         bool coveredExplicitly = false;
         bool coveredByInheritance = false;
         for (DWORD index = 0; index < size.AceCount; index++)
         {
            LPVOID entry = nullptr;
            if (!GetAce(dacl, index, &entry) || entry == nullptr)
               continue;

            const ACE_HEADER *header = (const ACE_HEADER *) entry;
            const bool inherited = (header->AceFlags & INHERITED_ACE) != 0;

            if (header->AceType == ACCESS_DENIED_ACE_TYPE)
            {
               const ACCESS_DENIED_ACE *deny = (const ACCESS_DENIED_ACE *) entry;
               if ((header->AceFlags & INHERIT_ONLY_ACE) == 0 && EqualSid((PSID) &deny->SidStart, sid) && (FileRights_(deny->Mask) & rights) != 0)
               {
                  if (inherited)
                     inheritedDeny = true;
                  else
                     explicitDeny = true;
               }
               continue;
            }

            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE) != 0)
               continue;

            const ACCESS_ALLOWED_ACE *allow = (const ACCESS_ALLOWED_ACE *) entry;
            if (!EqualSid((PSID) &allow->SidStart, sid))
               continue;
            if ((FileRights_(allow->Mask) & rights) != rights)
               continue;

            if (toEverythingBelow)
            {
               const BYTE both = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
               if ((header->AceFlags & both) != both || (header->AceFlags & NO_PROPAGATE_INHERIT_ACE) != 0)
                  continue;
            }

            if (inherited)
               coveredByInheritance = true;
            else
               coveredExplicitly = true;
         }

         return coveredExplicitly || (coveredByInheritance && !inheritedDeny);
      }

      String DeniedSentence_()
      {
         return _T("an explicit deny entry for the service on it takes away some of these rights, and outranks any allow entry; it was left as it is");
      }

      String InheritedDenySentence_()
      {
         return _T("an inherited deny entry for the service is on it; the entry added here comes before it, and takes effect");
      }

      // The DACL with one allow entry for sid added where Windows' canonical
      // order puts an explicit allow: after the explicit entries and before the
      // inherited ones. Nothing is merged into an existing entry and nothing is
      // taken out - an administrator's deny entry for the service included - so
      // the only change is the one line. nullptr, with detail, when it cannot be
      // built.
      PACL BuildAcl_(PACL existing, PSID sid, DWORD rights, BYTE flags, std::vector<BYTE> &buffer, String &detail)
      {
         ACL_SIZE_INFORMATION size;
         memset(&size, 0, sizeof(size));
         size.AclBytesInUse = sizeof(ACL);
         DWORD revision = ACL_REVISION;
         if (existing != nullptr)
         {
            if (!GetAclInformation(existing, &size, sizeof(size), AclSizeInformation))
            {
               detail = Formatter::Format(_T("its permissions could not be read (Windows error {0})"), (int) GetLastError());
               return nullptr;
            }
            if (existing->AclRevision == ACL_REVISION_DS)
               revision = ACL_REVISION_DS;
         }

         const DWORD entrySize = (DWORD) (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(sid));
         const DWORD aclSize = (size.AclBytesInUse + entrySize + sizeof(DWORD) - 1) & ~((DWORD) sizeof(DWORD) - 1);
         buffer.assign(aclSize, 0);
         PACL acl = (PACL) buffer.data();
         if (!InitializeAcl(acl, aclSize, revision))
         {
            detail = Formatter::Format(_T("the new permissions could not be built (Windows error {0})"), (int) GetLastError());
            return nullptr;
         }

         bool added = false;
         for (DWORD index = 0; existing != nullptr && index < size.AceCount; index++)
         {
            LPVOID entry = nullptr;
            if (!GetAce(existing, index, &entry) || entry == nullptr)
            {
               detail = Formatter::Format(_T("entry {0} of its permissions could not be read"), (int) index);
               return nullptr;
            }

            const ACE_HEADER *header = (const ACE_HEADER *) entry;
            if (!added && (header->AceFlags & INHERITED_ACE) != 0)
            {
               if (!AddAccessAllowedAceEx(acl, revision, flags, rights, sid))
               {
                  detail = Formatter::Format(_T("the entry could not be added (Windows error {0})"), (int) GetLastError());
                  return nullptr;
               }
               added = true;
            }

            if (!AddAce(acl, revision, MAXDWORD, entry, header->AceSize))
            {
               detail = Formatter::Format(_T("an existing entry could not be copied (Windows error {0})"), (int) GetLastError());
               return nullptr;
            }
         }

         if (!added && !AddAccessAllowedAceEx(acl, revision, flags, rights, sid))
         {
            detail = Formatter::Format(_T("the entry could not be added (Windows error {0})"), (int) GetLastError());
            return nullptr;
         }

         return acl;
      }

      struct SidRelease_
      {
         explicit SidRelease_(PSID held) : sid(held) {}
         // Releases what it holds when it goes: a copy would release it twice.
         SidRelease_(const SidRelease_ &) = delete;
         SidRelease_ &operator=(const SidRelease_ &) = delete;

         PSID sid;
         ~SidRelease_() { if (sid) LocalFree(sid); }
      };

      // Holds every folder from the drive's root down to a path open, without
      // sharing delete, so that none of them can be renamed, deleted or swapped
      // for a junction while a grant is written through the path - and refuses
      // a path that runs through a junction, a symbolic link or a mount point
      // already, or is not on a fixed local drive. Released when it goes.
      class PathPin_
      {
      public:
         PathPin_() : missing_(false) {}
         ~PathPin_() { Release(); }

         bool Pin(const String &path, String &why)
         {
            Release();
            missing_ = false;

            if (!IsDrivePath_(path))
            {
               why = Formatter::Format(_T("{0} is not a full path on a drive of this computer"), path);
               return false;
            }

            const String target = Normalised_(path);
            const String root = target.Left(3);
            if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED)
            {
               why = Formatter::Format(_T("{0} is not on a fixed local drive, and only those are granted"), target);
               return false;
            }

            String walked = root.Left(2);
            String rest = target.Mid(3);
            while (!rest.IsEmpty())
            {
               const int separator = rest.Find(_T('\\'));
               const String part = separator < 0 ? rest : rest.Left(separator);
               rest = separator < 0 ? String() : rest.Mid(separator + 1);
               if (part.IsEmpty())
                  continue;

               walked += _T("\\") + part;

               // FILE_LIST_DIRECTORY as well as the attributes: a handle that asks
               // for attributes alone takes no part in Windows' share-access check,
               // so its refusal to share delete binds nobody, and the folder could
               // be removed or renamed from under it (the internal suite's
               // ServiceAccount test found exactly that on the batch36 bench).
               HANDLE handle = CreateFileW(walked.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
               if (handle == INVALID_HANDLE_VALUE)
               {
                  const int error = (int) GetLastError();
                  missing_ = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
                  why = missing_
                     ? Formatter::Format(_T("{0} does not exist"), walked)
                     : Formatter::Format(_T("{0} could not be held while it is granted (Windows error {1}); run this again"), walked, error);
                  Release();
                  return false;
               }
               handles_.push_back(handle);

               BY_HANDLE_FILE_INFORMATION information;
               if (!GetFileInformationByHandle(handle, &information))
               {
                  why = Formatter::Format(_T("{0} could not be examined (Windows error {1})"), walked, (int) GetLastError());
                  Release();
                  return false;
               }

               if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
               {
                  why = Formatter::Format(_T("{0} is a junction, a symbolic link or a mount point, and a grant through it would reach wherever it points"), walked);
                  Release();
                  return false;
               }
            }

            return true;
         }

         // After a failed Pin: whether it failed because the path is not there.
         bool Missing() const { return missing_; }

         void Release()
         {
            for (HANDLE handle : handles_)
               CloseHandle(handle);
            handles_.clear();
         }

      private:
         PathPin_(const PathPin_ &);
         PathPin_ &operator=(const PathPin_ &);

         std::vector<HANDLE> handles_;
         bool missing_;
      };

      // Grants sid the rights on one file, through a handle opened without
      // following links, with every folder above it held (PathPin_), and only
      // when it is a file with one name: a hard link elsewhere would carry the
      // grant to wherever else the file is. The protection of its permissions
      // stays what it was.
      ServiceAccount::GrantResult GrantFile_(const String &file, const String &sidText, DWORD rights, String &detail)
      {
         detail.Empty();

         PSID sid = nullptr;
         if (!ConvertStringSidToSidW(sidText.c_str(), &sid))
         {
            detail = Formatter::Format(_T("{0} is not a SID"), sidText);
            return ServiceAccount::GrantFailed;
         }
         SidRelease_ releaseSid(sid);

         PathPin_ pin;
         const String folder = FolderOf_(Normalised_(file));
         if (folder.IsEmpty() || !pin.Pin(folder, detail))
         {
            if (folder.IsEmpty())
               detail = _T("it is not in a folder");
            return ServiceAccount::GrantFailed;
         }

         HANDLE handle = CreateFileW(Normalised_(file).c_str(), READ_CONTROL | WRITE_DAC | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
         if (handle == INVALID_HANDLE_VALUE)
         {
            detail = Formatter::Format(_T("it could not be opened (Windows error {0})"), (int) GetLastError());
            return ServiceAccount::GrantFailed;
         }

         struct HandleRelease_
         {
            explicit HandleRelease_(HANDLE held) : handle(held) {}
            // Releases what it holds when it goes: a copy would release it twice.
            HandleRelease_(const HandleRelease_ &) = delete;
            HandleRelease_ &operator=(const HandleRelease_ &) = delete;

            HANDLE handle;
            ~HandleRelease_() { CloseHandle(handle); }
         } releaseHandle(handle);

         BY_HANDLE_FILE_INFORMATION information;
         if (!GetFileInformationByHandle(handle, &information))
         {
            detail = Formatter::Format(_T("it could not be examined (Windows error {0})"), (int) GetLastError());
            return ServiceAccount::GrantFailed;
         }
         if ((information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
         {
            detail = _T("it is a folder, a junction or a link, not a file");
            return ServiceAccount::GrantFailed;
         }
         if (information.nNumberOfLinks != 1)
         {
            detail = Formatter::Format(_T("it has {0} names on the disk (hard links), and a grant on it would reach it under every one of them; it was left as it is"), (int) information.nNumberOfLinks);
            return ServiceAccount::GrantFailed;
         }

         Permissions_ permissions;
         const DWORD read = GetSecurityInfo(handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &permissions.dacl, nullptr, &permissions.descriptor);
         if (read != ERROR_SUCCESS)
         {
            detail = Formatter::Format(_T("its permissions could not be read (Windows error {0})"), (int) read);
            return ServiceAccount::GrantFailed;
         }

         bool explicitDeny = false;
         bool inheritedDeny = false;
         const bool covered = Covers_(permissions.dacl, sid, rights, false, explicitDeny, inheritedDeny);
         if (explicitDeny)
         {
            detail = DeniedSentence_();
            return ServiceAccount::GrantFailed;
         }
         if (covered)
            return ServiceAccount::GrantAlreadyThere;

         std::vector<BYTE> buffer;
         PACL acl = BuildAcl_(permissions.dacl, sid, rights, 0, buffer, detail);
         if (acl == nullptr)
            return ServiceAccount::GrantFailed;

         const SECURITY_INFORMATION which = DACL_SECURITY_INFORMATION |
            (IsProtected_(permissions) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION);
         const DWORD written = SetSecurityInfo(handle, SE_FILE_OBJECT, which, nullptr, nullptr, acl, nullptr);
         if (written != ERROR_SUCCESS)
         {
            detail = Formatter::Format(_T("its permissions could not be written (Windows error {0})"), (int) written);
            return ServiceAccount::GrantFailed;
         }

         if (inheritedDeny)
            detail = InheritedDenySentence_();
         return ServiceAccount::GrantMade;
      }

      // The account the hMailServer service logs on as, and whether its SID
      // type puts NT SERVICE\hMailServer in its token. False when the service
      // cannot be read.
      bool ReadService_(String &logon, DWORD &sidType)
      {
         logon.Empty();
         sidType = SERVICE_SID_TYPE_NONE;

         SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
         if (manager == nullptr)
            return false;

         bool read = false;
         SC_HANDLE service = OpenServiceW(manager, L"hMailServer", SERVICE_QUERY_CONFIG);
         if (service != nullptr)
         {
            DWORD needed = 0;
            QueryServiceConfigW(service, nullptr, 0, &needed);
            if (needed > 0)
            {
               std::vector<BYTE> buffer(needed);
               QUERY_SERVICE_CONFIGW *config = (QUERY_SERVICE_CONFIGW *) buffer.data();
               if (QueryServiceConfigW(service, config, needed, &needed))
               {
                  read = true;
                  logon = config->lpServiceStartName ? config->lpServiceStartName : L"LocalSystem";
               }
            }

            needed = 0;
            QueryServiceConfig2W(service, SERVICE_CONFIG_SERVICE_SID_INFO, nullptr, 0, &needed);
            if (needed > 0)
            {
               std::vector<BYTE> buffer(needed);
               if (QueryServiceConfig2W(service, SERVICE_CONFIG_SERVICE_SID_INFO, buffer.data(), needed, &needed))
                  sidType = ((SERVICE_SID_INFO *) buffer.data())->dwServiceSidType;
            }

            CloseServiceHandle(service);
         }

         CloseServiceHandle(manager);
         return read;
      }

      bool IsLocalSystemName_(const String &account)
      {
         return account.CompareNoCase(_T("LocalSystem")) == 0 || account.CompareNoCase(_T(".\\LocalSystem")) == 0 ||
                account.CompareNoCase(_T("NT AUTHORITY\\SYSTEM")) == 0;
      }

      // The program folder, from where this binary is - {app}\Bin\hMailServer.exe
      // - and never from [Directories] ProgramFolder, which the grant does not
      // trust. False when the binary is not in a folder named Bin, or that is
      // at a drive's root.
      bool ProgramFolderOfThisBinary_(String &program, String &executable)
      {
         std::vector<wchar_t> buffer(32768);
         const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), (DWORD) buffer.size());
         if (length == 0 || length >= buffer.size())
            return false;

         executable = Resolved_(String(buffer.data()));
         const String bin = FolderOf_(executable);
         const int separator = bin.ReverseFind(_T('\\'));
         if (bin.GetLength() <= 3 || separator < 2 || bin.Mid(separator + 1).CompareNoCase(_T("Bin")) != 0)
            return false;

         program = FolderOf_(bin);
         return program.GetLength() > 3;
      }

      // The program folder's own folders the service writes, granted Modify
      // with no approval: the installer makes them, and nobody but an
      // administrator can make a folder of that name there.
      const wchar_t *const WritableDefaults_[] = { L"Data", L"Logs", L"Temp", L"Database" };

      // The folders no grant may reach, whatever an approval says.
      struct Guarded_
      {
         // The installation's own folder. Only its Data, Logs, Temp and
         // Database folders, and what is in them, may be granted; the folder
         // itself, what is beside those four in it, and anything holding it,
         // may not.
         String program;

         // Is, holds or is in: refused. The Windows folder, the Program Files
         // folders, the user profiles, and ProgramData\Microsoft (the Start
         // menu and the startup folder of every user).
         std::vector<String> nowhereNear;

         // Is or holds: refused. ProgramData, whose own folders may be approved.
         std::vector<String> notAbove;
      };

      Guarded_ MachineGuarded_(const String &program)
      {
         Guarded_ guarded;
         guarded.program = program;

         wchar_t windows[MAX_PATH];
         const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
         if (length > 0 && length < MAX_PATH)
            guarded.nowhereNear.push_back(Resolved_(String(windows)));

         const String programFiles = KnownFolder_(FOLDERID_ProgramFiles);
         const String programFilesX86 = KnownFolder_(FOLDERID_ProgramFilesX86);
         const String profiles = KnownFolder_(FOLDERID_UserProfiles);
         const String programData = KnownFolder_(FOLDERID_ProgramData);

         if (!programFiles.IsEmpty())
            guarded.nowhereNear.push_back(programFiles);
         if (!programFilesX86.IsEmpty())
            guarded.nowhereNear.push_back(programFilesX86);
         if (!profiles.IsEmpty())
            guarded.nowhereNear.push_back(profiles);
         if (!programData.IsEmpty())
         {
            guarded.notAbove.push_back(programData);
            guarded.nowhereNear.push_back(programData + _T("\\Microsoft"));
         }

         return guarded;
      }

      // Why a folder may not be granted, or false when it may. Judged on the
      // path's spelling alone - the links on the way are PathPin_'s to find -
      // so that the tester can ask about folders that are not there.
      bool RefusedLocation_(const String &path, const Guarded_ &guarded, String &why)
      {
         why.Empty();

         if (!IsDrivePath_(path))
         {
            why = _T("it is not a full path on a drive of this computer; a network share, a device path, a relative path or a stream is never granted");
            return true;
         }

         const String target = Normalised_(path);
         if (target.GetLength() <= 3)
         {
            why = _T("it is the root of a drive");
            return true;
         }

         if (!guarded.program.IsEmpty())
         {
            if (IsUnder_(guarded.program, target))
            {
               why = _T("it is, or holds, the program folder, where the programs an administrator runs are");
               return true;
            }

            if (IsUnder_(target, guarded.program))
            {
               for (const wchar_t *name : WritableDefaults_)
               {
                  if (IsUnder_(target, FileUtilities::Combine(guarded.program, String(name))))
                     return false;
               }

               why = _T("it is in the program folder, outside its Data, Logs, Temp and Database folders");
               return true;
            }
         }

         for (const String &folder : guarded.nowhereNear)
         {
            if (IsUnder_(target, folder) || IsUnder_(folder, target))
            {
               why = Formatter::Format(_T("it is, holds or is in {0}, which belongs to Windows, to other programs or to the users of this computer"), folder);
               return true;
            }
         }

         for (const String &folder : guarded.notAbove)
         {
            if (IsUnder_(folder, target))
            {
               why = Formatter::Format(_T("it is or holds {0}"), folder);
               return true;
            }
         }

         return false;
      }

      // A path the configuration names: what it is for, whether the service
      // writes there, and whether it is a file (which the service only reads).
      struct Named_
      {
         String path;
         String what;
         bool writes;
         bool isFile;
      };

      // One folder the grant makes, handed down to what is in it.
      struct Wanted_
      {
         String path;
         DWORD rights;
         String what;
      };

      // What the grant will do and say. The folders come from trusted places
      // alone - the program folder and the approved list; the configuration
      // decides only what is SAID about the paths it names.
      struct Plan_
      {
         std::vector<Wanted_> folders;
         std::vector<Named_> notApproved;
         std::vector<Named_> outside;
         std::vector<Named_> filesInside;
         std::vector<std::pair<Named_, String> > refused;
         std::vector<std::pair<String, String> > refusedApprovals;
         std::vector<String> notes;
      };

      bool Reaches_(const std::vector<Wanted_> &folders, const String &path, bool writes)
      {
         for (const Wanted_ &folder : folders)
         {
            if (!IsUnder_(path, folder.path))
               continue;
            if (writes && folder.rights != ServiceAccount::RightsModify)
               continue;
            return true;
         }
         return false;
      }

      // The plan, from the program folder, the approved list and the paths the
      // configuration names. Nothing here touches the disk or the registry.
      void PlanGrants_(const String &program, const std::vector<std::pair<String, DWORD> > &approved,
         const std::vector<Named_> &named, const Guarded_ &guarded, Plan_ &plan)
      {
         if (!program.IsEmpty())
         {
            plan.folders.push_back({ FileUtilities::Combine(program, _T("Data")), ServiceAccount::RightsModify, _T("the data folder: the message store, and its ACME, Certificates, DKIM, Updates and other folders") });
            plan.folders.push_back({ FileUtilities::Combine(program, _T("Logs")), ServiceAccount::RightsModify, _T("the log folder, where the crash dumps and the crash record are written too") });
            plan.folders.push_back({ FileUtilities::Combine(program, _T("Temp")), ServiceAccount::RightsModify, _T("the temp folder") });
            plan.folders.push_back({ FileUtilities::Combine(program, _T("Database")), ServiceAccount::RightsModify, _T("the database folder, where the built-in SQL Server Compact database is kept") });
         }

         for (const auto &entry : approved)
         {
            String why;
            if (RefusedLocation_(entry.first, guarded, why))
            {
               plan.refusedApprovals.push_back(std::make_pair(entry.first, why));
               continue;
            }

            const DWORD rights = entry.second == ServiceAccount::RightsModify ? ServiceAccount::RightsModify : ServiceAccount::RightsReadExecute;
            plan.folders.push_back({ Normalised_(entry.first), rights, _T("approved by an administrator") });
         }

         for (const Named_ &item : named)
         {
            String path = item.path;
            path.Trim();
            if (path.IsEmpty())
               continue;

            Named_ trimmed = item;
            trimmed.path = path;

            // Something the service only reads, in one of the program folder's own
            // folders - the event scripts in Events, above all - is reached by the
            // Read those folders are granted (RunGrantCommand).
            const bool readInProgram = !item.writes && !program.IsEmpty() && IsUnder_(path, program) && !SamePath_(path, program);

            if (readInProgram || Reaches_(plan.folders, path, item.writes))
            {
               if (item.isFile)
                  plan.filesInside.push_back(trimmed);
               continue;
            }

            if (item.isFile)
            {
               plan.outside.push_back(trimmed);
               continue;
            }

            if (FileUtilities::IsUNCPath(path))
            {
               plan.notes.push_back(Formatter::Format(_T("{0} ({1}) is a network share. The service reaches it as this computer's account (DOMAIN\\COMPUTER$), as LocalSystem did; grant that account on the share. Nothing is granted here."), path, item.what));
               continue;
            }

            String why;
            if (RefusedLocation_(path, guarded, why))
            {
               plan.refused.push_back(std::make_pair(trimmed, why));
               continue;
            }

            bool listed = false;
            for (Named_ &already : plan.notApproved)
            {
               if (!SamePath_(already.path, path))
                  continue;
               already.writes = already.writes || item.writes;
               listed = true;
            }
            if (!listed)
               plan.notApproved.push_back(trimmed);
         }
      }

      bool IsTrustedWriter_(PSID sid)
      {
         static const String trustedInstaller = _T("S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464");
         return sid != nullptr && (IsWellKnownSid(sid, WinBuiltinAdministratorsSid) || IsWellKnownSid(sid, WinLocalSystemSid) ||
            SidText_(sid).CompareNoCase(trustedInstaller) == 0);
      }

      // Whether only Administrators, SYSTEM and TrustedInstaller may change a
      // registry key: its owner, and every allow entry that applies to it and
      // gives a right to change it. False, with why, when anybody else may.
      bool KeyWritableOnlyByAdministrators_(HKEY key, String &why)
      {
         PSECURITY_DESCRIPTOR descriptor = nullptr;
         PSID owner = nullptr;
         PACL dacl = nullptr;
         const DWORD result = GetSecurityInfo(key, SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &descriptor);
         if (result != ERROR_SUCCESS)
         {
            why = Formatter::Format(_T("its permissions could not be read (Windows error {0})"), (int) result);
            return false;
         }

         bool trusted = true;
         if (!IsTrustedWriter_(owner))
         {
            trusted = false;
            why = Formatter::Format(_T("its owner is {0}"), SidText_(owner));
         }
         else if (dacl == nullptr)
         {
            trusted = false;
            why = _T("it has no permissions at all, so anybody may change it");
         }
         else
         {
            ACL_SIZE_INFORMATION size;
            memset(&size, 0, sizeof(size));
            if (!GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation))
            {
               trusted = false;
               why = _T("its permissions could not be read");
            }

            for (DWORD index = 0; trusted && index < size.AceCount; index++)
            {
               LPVOID entry = nullptr;
               if (!GetAce(dacl, index, &entry) || entry == nullptr)
                  continue;

               const ACE_HEADER *header = (const ACE_HEADER *) entry;
               if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE) != 0)
                  continue;

               const ACCESS_ALLOWED_ACE *allow = (const ACCESS_ALLOWED_ACE *) entry;
               DWORD mask = allow->Mask;
               GENERIC_MAPPING mapping = { KEY_READ, KEY_WRITE, KEY_EXECUTE, KEY_ALL_ACCESS };
               MapGenericMask(&mask, &mapping);

               const DWORD changes = KEY_SET_VALUE | KEY_CREATE_SUB_KEY | WRITE_DAC | WRITE_OWNER | DELETE;
               if ((mask & changes) != 0 && !IsTrustedWriter_((PSID) &allow->SidStart))
               {
                  trusted = false;
                  why = Formatter::Format(_T("{0} may change it"), SidText_((PSID) &allow->SidStart));
               }
            }
         }

         LocalFree(descriptor);

         if (!trusted)
            why = Formatter::Format(_T("HKEY_LOCAL_MACHINE\\{0} is not trusted, so no folder in it is granted: {1}. An administrator gives it back to Administrators and SYSTEM alone, or deletes it, and runs this again"),
               ServiceAccount::ApprovedPathsKey(), why);
         return trusted;
      }

      // The folders approved in HKLM\ApprovedPathsKey(). False, with why, when
      // the key is there and somebody but administrators may change it - then
      // nothing in it is trusted. True, with none, when it is not there.
      bool ReadApproved_(std::vector<std::pair<String, DWORD> > &approved, String &why)
      {
         approved.clear();

         HKEY key = nullptr;
         const LONG opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, ServiceAccount::ApprovedPathsKey().c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key);
         if (opened == ERROR_FILE_NOT_FOUND)
            return true;
         if (opened != ERROR_SUCCESS)
         {
            why = Formatter::Format(_T("HKEY_LOCAL_MACHINE\\{0} could not be read (Windows error {1})"), ServiceAccount::ApprovedPathsKey(), (int) opened);
            return false;
         }

         const bool trusted = KeyWritableOnlyByAdministrators_(key, why);
         for (DWORD index = 0; trusted; index++)
         {
            std::vector<wchar_t> name(16384);
            DWORD nameLength = (DWORD) name.size();
            wchar_t data[64];
            DWORD dataBytes = sizeof(data) - sizeof(wchar_t);
            DWORD type = 0;

            const LONG result = RegEnumValueW(key, index, name.data(), &nameLength, nullptr, &type, (LPBYTE) data, &dataBytes);
            if (result == ERROR_NO_MORE_ITEMS)
               break;
            if (result == ERROR_MORE_DATA)
               continue;
            if (result != ERROR_SUCCESS)
               break;
            if (type != REG_SZ)
               continue;

            data[dataBytes / sizeof(wchar_t)] = 0;
            String rights(data);
            rights.Trim();

            if (rights.CompareNoCase(_T("Modify")) == 0)
               approved.push_back(std::make_pair(String(name.data()), (DWORD) ServiceAccount::RightsModify));
            else if (rights.CompareNoCase(_T("Read")) == 0)
               approved.push_back(std::make_pair(String(name.data()), (DWORD) ServiceAccount::RightsReadExecute));
         }

         RegCloseKey(key);
         return trusted;
      }

      // Records one approval, making the key - owned by Administrators, and
      // changed by them and SYSTEM alone - when it is not there.
      bool WriteApproval_(const String &path, DWORD rights, String &why)
      {
         PSECURITY_DESCRIPTOR descriptor = nullptr;
         if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAD:P(A;OICI;KA;;;SY)(A;OICI;KA;;;BA)(A;OICI;KR;;;BU)", SDDL_REVISION_1, &descriptor, nullptr))
         {
            why = Formatter::Format(_T("the key's permissions could not be built (Windows error {0})"), (int) GetLastError());
            return false;
         }

         SECURITY_ATTRIBUTES attributes;
         attributes.nLength = sizeof(attributes);
         attributes.lpSecurityDescriptor = descriptor;
         attributes.bInheritHandle = FALSE;

         HKEY key = nullptr;
         DWORD disposition = 0;
         const LONG created = RegCreateKeyExW(HKEY_LOCAL_MACHINE, ServiceAccount::ApprovedPathsKey().c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_READ | KEY_SET_VALUE | KEY_WOW64_64KEY, &attributes, &key, &disposition);
         LocalFree(descriptor);
         if (created != ERROR_SUCCESS)
         {
            why = Formatter::Format(_T("HKEY_LOCAL_MACHINE\\{0} could not be opened for writing (Windows error {1})"), ServiceAccount::ApprovedPathsKey(), (int) created);
            return false;
         }

         if (disposition == REG_OPENED_EXISTING_KEY && !KeyWritableOnlyByAdministrators_(key, why))
         {
            RegCloseKey(key);
            return false;
         }

         const String data = rights == ServiceAccount::RightsModify ? String(_T("Modify")) : String(_T("Read"));
         const LONG written = RegSetValueExW(key, path.c_str(), 0, REG_SZ, (const BYTE *) data.c_str(), (DWORD) ((data.GetLength() + 1) * sizeof(wchar_t)));
         RegCloseKey(key);

         if (written != ERROR_SUCCESS)
         {
            why = Formatter::Format(_T("the approval could not be written (Windows error {0})"), (int) written);
            return false;
         }
         return true;
      }

      // What the database names, added to named: the backup folder, the
      // settings in hm_inisettings that name folders and files - read there and
      // not from the file's copy, which the service's own account does not
      // refresh - and the certificate and key files. False, with why, when the
      // database cannot be read, which on a new installation, before its
      // database exists, is the ordinary case; the settings are then read from
      // the file's copy.
      bool ReadConfiguredPaths_(std::vector<Named_> &named, String &why)
      {
         IniFileSettings *ini = IniFileSettings::Instance();
         std::map<String, String> stored;
         std::shared_ptr<DALConnection> connection;

         if (!ini->GetDatabaseSettingsExists())
         {
            why = _T("hMailServer.INI names no database yet");
         }
         else
         {
            connection = DALConnectionFactory::CreateConnection(SchemaUpgrader::SettingsFromIni());
            String error;
            if (!connection || connection->Connect(error) != DALConnection::Connected)
            {
               why = Formatter::Format(_T("the database could not be opened ({0})"), error.IsEmpty() ? String(_T("no answer")) : error);
               connection.reset();
            }
         }

         if (connection)
         {
            SQLCommand rows(_T("select inisettingname, inisettingvalue from hm_inisettings"));
            std::shared_ptr<DALRecordset> settingRows = connection->CreateRecordset();
            if (settingRows && settingRows->Open(connection, rows))
            {
               while (!settingRows->IsEOF())
               {
                  stored[settingRows->GetStringValue("inisettingname")] = settingRows->GetStringValue("inisettingvalue");
                  settingRows->MoveNext();
               }
            }

            SQLCommand backup(_T("select settingstring from hm_settings where settingname = @SETTINGNAME"));
            backup.AddParameter("@SETTINGNAME", String(_T("backupdestination")));
            std::shared_ptr<DALRecordset> settings = connection->CreateRecordset();
            if (settings && settings->Open(connection, backup) && !settings->IsEOF())
               named.push_back({ settings->GetStringValue("settingstring"), _T("the backup folder"), true, false });

            // The folder the PST import reads Outlook data files from
            // (PstImportPath.cpp), an hm_settings row like the backup folder's.
            // The service only reads there.
            SQLCommand pstImport(_T("select settingstring from hm_settings where settingname = @SETTINGNAME"));
            pstImport.AddParameter("@SETTINGNAME", String(PROPERTY_PST_IMPORT_DIRECTORY));
            std::shared_ptr<DALRecordset> pstRows = connection->CreateRecordset();
            if (pstRows && pstRows->Open(connection, pstImport) && !pstRows->IsEOF())
               named.push_back({ pstRows->GetStringValue("settingstring"), _T("the PST import folder (PstImportDirectory)"), false, false });

            SQLCommand certificates(_T("select sslcertificatename, sslcertificatefile, sslprivatekeyfile from hm_sslcertificates"));
            std::shared_ptr<DALRecordset> certificateRows = connection->CreateRecordset();
            if (certificateRows && certificateRows->Open(connection, certificates))
            {
               while (!certificateRows->IsEOF())
               {
                  const String name = certificateRows->GetStringValue("sslcertificatename");
                  named.push_back({ certificateRows->GetStringValue("sslcertificatefile"), Formatter::Format(_T("the certificate of \"{0}\""), name), false, true });
                  named.push_back({ certificateRows->GetStringValue("sslprivatekeyfile"), Formatter::Format(_T("the private key of \"{0}\""), name), false, true });
                  certificateRows->MoveNext();
               }
            }

            SQLCommand domains(_T("select domainname, domaindkimprivatekeyfile, domaindkimsecondaryprivatekeyfile from hm_domains"));
            std::shared_ptr<DALRecordset> domainRows = connection->CreateRecordset();
            if (domainRows && domainRows->Open(connection, domains))
            {
               while (!domainRows->IsEOF())
               {
                  const String name = domainRows->GetStringValue("domainname");
                  named.push_back({ domainRows->GetStringValue("domaindkimprivatekeyfile"), Formatter::Format(_T("the DKIM key of {0}"), name), false, true });
                  named.push_back({ domainRows->GetStringValue("domaindkimsecondaryprivatekeyfile"), Formatter::Format(_T("the next DKIM key of {0}"), name), false, true });
                  domainRows->MoveNext();
               }
            }

            connection->Disconnect();
         }

         auto setting = [&stored](const String &name, const String &fromFile) -> String
         {
            auto found = stored.find(name);
            return found != stored.end() ? (*found).second : fromFile;
         };

         named.push_back({ setting(_T("ArchiveDir"), ini->GetArchiveDir()), _T("the archive folder (ArchiveDir)"), true, false });
         named.push_back({ setting(_T("AcmeCertificateDirectory"), ini->GetAcmeCertificateDirectory()), _T("the ACME certificate folder (AcmeCertificateDirectory)"), true, false });
         named.push_back({ setting(_T("RestApiCertificateFile"), ini->GetRestApiCertificateFile()), _T("the REST API's certificate (RestApiCertificateFile)"), false, true });
         named.push_back({ setting(_T("RestApiPrivateKeyFile"), ini->GetRestApiPrivateKeyFile()), _T("the REST API's private key (RestApiPrivateKeyFile)"), false, true });
         named.push_back({ setting(_T("MetricsServerCertificateFile"), ini->GetMetricsServerCertificateFile()), _T("the metrics listener's certificate (MetricsServerCertificateFile)"), false, true });
         named.push_back({ setting(_T("MetricsServerPrivateKeyFile"), ini->GetMetricsServerPrivateKeyFile()), _T("the metrics listener's private key (MetricsServerPrivateKeyFile)"), false, true });
         named.push_back({ setting(_T("WebServicesCertificateFile"), ini->GetWebServicesCertificateFile()), _T("the web services' certificate (WebServicesCertificateFile)"), false, true });
         named.push_back({ setting(_T("WebServicesPrivateKeyFile"), ini->GetWebServicesPrivateKeyFile()), _T("the web services' private key (WebServicesPrivateKeyFile)"), false, true });

         return why.IsEmpty();
      }

      // The immediate subfolders of a folder, reparse points left out: a
      // junction is somebody else's folder, reached through this one.
      std::vector<String> Subfolders_(const String &directory)
      {
         std::vector<String> found;
         WIN32_FIND_DATAW data;
         HANDLE search = FindFirstFileExW(FileUtilities::Combine(directory, _T("*")).c_str(), FindExInfoBasic, &data, FindExSearchLimitToDirectories, nullptr, 0);
         if (search == INVALID_HANDLE_VALUE)
            return found;

         do
         {
            const std::wstring name = data.cFileName;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
               continue;
            if (name == L"." || name == L"..")
               continue;
            found.push_back(FileUtilities::Combine(directory, String(name.c_str())));
         } while (FindNextFileW(search, &data));

         FindClose(search);
         return found;
      }

      // The files, not the folders or links, directly in a folder.
      std::vector<String> FilesIn_(const String &directory)
      {
         std::vector<String> found;
         WIN32_FIND_DATAW data;
         HANDLE search = FindFirstFileW(FileUtilities::Combine(directory, _T("*")).c_str(), &data);
         if (search == INVALID_HANDLE_VALUE)
            return found;

         do
         {
            if ((data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
               continue;
            found.push_back(FileUtilities::Combine(directory, String(data.cFileName)));
         } while (FindNextFileW(search, &data));

         FindClose(search);
         return found;
      }

      // The rights as a column of the report, six characters wide.
      String RightsName_(DWORD rights)
      {
         return rights == ServiceAccount::RightsModify ? String(_T("Modify")) : String(_T("Read  "));
      }

      // Whether an allow entry that applies to path lets one of sids write it:
      // add or change files in a folder, or change a file. The name of the first
      // that may, or empty. What the grant checks the program folder and
      // hMailServer.INI against, and says - it removes nobody's entry.
      String WritableBy_(const String &path, const std::vector<String> &sids)
      {
         Permissions_ permissions;
         String error;
         if (!ReadPermissions_(path, permissions, error) || permissions.dacl == nullptr)
            return String();

         ACL_SIZE_INFORMATION size;
         if (!GetAclInformation(permissions.dacl, &size, sizeof(size), AclSizeInformation))
            return String();

         const DWORD writes = FILE_WRITE_DATA | FILE_APPEND_DATA | WRITE_DAC | WRITE_OWNER | DELETE;
         for (DWORD index = 0; index < size.AceCount; index++)
         {
            LPVOID entry = nullptr;
            if (!GetAce(permissions.dacl, index, &entry) || entry == nullptr)
               continue;

            const ACE_HEADER *header = (const ACE_HEADER *) entry;
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE) != 0)
               continue;

            const ACCESS_ALLOWED_ACE *allow = (const ACCESS_ALLOWED_ACE *) entry;
            if ((FileRights_(allow->Mask) & writes) == 0)
               continue;

            const String who = SidText_((PSID) &allow->SidStart);
            for (const String &sid : sids)
            {
               if (who.CompareNoCase(sid) == 0)
                  return who;
            }
         }

         return String();
      }

      // The box --confirm asks in, on the desktop of this elevated process.
      // True for Yes.
      bool AskToProceed_(const Plan_ &plan, const String &databaseUnread)
      {
         String text = _T("hMailServer.exe /GrantServiceAccess is about to grant NT SERVICE\\hMailServer, the service's own account, its folders.\r\n");

         if (!databaseUnread.IsEmpty())
         {
            text += Formatter::Format(_T("\r\nThe database could not be read by this account ({0}), so the certificate and key files, the backup folder and the PST import folder it names are not known here: one outside the folders granted would not be reached by the service once it runs as NT SERVICE\\hMailServer. A new installation's database names none. Yes grants the folders as they are.\r\n"),
               databaseUnread);
         }

         if (!plan.notApproved.empty())
         {
            text += _T("\r\nThe configuration names these folders outside the program folder, and no administrator has approved them for the service:\r\n");
            for (const Named_ &folder : plan.notApproved)
               text += Formatter::Format(_T("    {0} - {1} ({2})\r\n"), folder.path, folder.what, folder.writes ? String(_T("Modify")) : String(_T("Read")));
            text += Formatter::Format(_T("Yes approves each of them - recorded under HKEY_LOCAL_MACHINE\\{0} - and grants it. Approve only folders you recognise: the service may then write in each.\r\n"), ServiceAccount::ApprovedPathsKey());
         }

         if (!plan.outside.empty())
         {
            text += _T("\r\nThese key and certificate files are outside every folder the service reaches, and are not granted: TLS or DKIM signing that uses them stops while the service runs as NT SERVICE\\hMailServer, until each is moved into the data folder's Certificates folder or its folder is approved for Read.\r\n");
            for (const Named_ &file : plan.outside)
               text += Formatter::Format(_T("    {0} - {1}\r\n"), file.path, file.what);
         }

         if (!plan.refused.empty())
         {
            text += _T("\r\nThese are named too, and are never granted to the service, approved or not; the configuration has to change before the service can reach them:\r\n");
            for (const auto &refusal : plan.refused)
               text += Formatter::Format(_T("    {0} - {1}: {2}\r\n"), refusal.first.path, refusal.first.what, refusal.second);
         }

         text += _T("\r\nContinue? No changes nothing at all.");

         const int answer = MessageBoxW(nullptr, text.c_str(), L"hMailServer - grant the service account its folders",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND | MB_TOPMOST);
         return answer == IDYES;
      }

      // The lists of what is not granted, in the form the installer's log and
      // the Control Panel's report show.
      void SayWhatIsNotGranted_(const Plan_ &plan)
      {
         if (!plan.notApproved.empty())
         {
            SayLine_(false, _T("Named by the configuration, outside the program folder, and not approved for the service, so not granted:"));
            for (const Named_ &folder : plan.notApproved)
               SayLine_(false, Formatter::Format(_T("  NOT APPROVED    {0}: {1}"), folder.what, folder.path));
            SayLine_(false, _T("  Approve one with hMailServer.exe --approve-modify \"<folder>\" /GrantServiceAccess (--approve-read for one it only reads), or run this with --confirm to be asked about each."));
         }

         if (!plan.outside.empty())
         {
            SayLine_(false, _T("Key and certificate files outside every folder above, so not granted - the service cannot read them:"));
            for (const Named_ &file : plan.outside)
               SayLine_(false, Formatter::Format(_T("  OUTSIDE         {0}: {1}"), file.what, file.path));

            const String certificates = ServiceAccount::CertificateDirectory();
            SayLine_(false, certificates.IsEmpty()
               ? String(_T("  Move them into the data folder, or approve their folder with --approve-read \"<folder>\"."))
               : Formatter::Format(_T("  Move them into {0}, which the service reaches, or approve their folder with --approve-read \"<folder>\"."), certificates));
         }

         if (!plan.refused.empty())
         {
            SayLine_(false, _T("Named by the configuration and never granted to the service, approved or not:"));
            for (const auto &refusal : plan.refused)
               SayLine_(false, Formatter::Format(_T("  REFUSED         {0}: {1} - {2}"), refusal.first.what, refusal.first.path, refusal.second));
         }
      }
   }

   String
   ServiceAccount::ProcessAccountName()
   {
      std::vector<BYTE> buffer;
      PSID sid = ProcessUserSid_(buffer);
      if (sid == nullptr)
         return String();

      wchar_t name[256];
      wchar_t domain[256];
      DWORD nameLength = 256;
      DWORD domainLength = 256;
      SID_NAME_USE use;
      if (LookupAccountSidW(nullptr, sid, name, &nameLength, domain, &domainLength, &use))
         return domain[0] != 0 ? String(domain) + _T("\\") + String(name) : String(name);

      // An account the lookup cannot name - a domain account while the domain
      // controller is away - is still something: its SID.
      return SidText_(sid);
   }

   bool
   ServiceAccount::ProcessIsMostPrivileged()
   {
      std::vector<BYTE> buffer;
      PSID sid = ProcessUserSid_(buffer);
      return sid != nullptr && IsWellKnownSid(sid, WinLocalSystemSid);
   }

   bool
   ServiceAccount::ProcessIsAdministrator()
   {
      BYTE administrators[SECURITY_MAX_SID_SIZE];
      DWORD size = sizeof(administrators);
      if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &size))
         return false;

      // The primary token when the thread is not impersonating, which no thread
      // of this server does: the group has to be enabled, which it is for
      // LocalSystem and for an administrator's service logon, and is not for a
      // virtual account, which is not a member at all.
      BOOL member = FALSE;
      if (!CheckTokenMembership(nullptr, administrators, &member))
         return false;
      return member != FALSE;
   }

   String
   ServiceAccount::ServiceSid(const String &serviceName)
   {
      std::wstring upper(serviceName.c_str());
      if (!upper.empty())
         CharUpperBuffW(&upper[0], (DWORD) upper.size());

      unsigned char digest[EVP_MAX_MD_SIZE];
      unsigned int digestLength = 0;
      if (EVP_Digest(upper.data(), upper.size() * sizeof(wchar_t), digest, &digestLength, EVP_sha1(), nullptr) != 1 || digestLength < 20)
         return String();

      unsigned int parts[5];
      for (int part = 0; part < 5; part++)
      {
         const unsigned char *bytes = digest + part * 4;
         parts[part] = (unsigned int) bytes[0] | ((unsigned int) bytes[1] << 8) | ((unsigned int) bytes[2] << 16) | ((unsigned int) bytes[3] << 24);
      }

      String sid;
      sid.Format(_T("S-1-5-80-%u-%u-%u-%u-%u"), parts[0], parts[1], parts[2], parts[3], parts[4]);
      return sid;
   }

   String
   ServiceAccount::CertificateDirectory()
   {
      String program;
      String executable;
      if (!ProgramFolderOfThisBinary_(program, executable))
         return String();
      return FileUtilities::Combine(FileUtilities::Combine(program, _T("Data")), _T("Certificates"));
   }

   bool
   ServiceAccount::HasGrant(const String &path, const String &sid, DWORD rights, bool toEverythingBelow, String &error)
   {
      PSID binary = nullptr;
      if (!ConvertStringSidToSidW(sid.c_str(), &binary))
      {
         error = Formatter::Format(_T("{0} is not a SID"), sid);
         return false;
      }

      Permissions_ permissions;
      bool covered = false;
      if (ReadPermissions_(path, permissions, error))
      {
         bool explicitDeny = false;
         bool inheritedDeny = false;
         covered = Covers_(permissions.dacl, binary, rights, toEverythingBelow, explicitDeny, inheritedDeny) && !explicitDeny;
      }

      LocalFree(binary);
      return covered;
   }

   ServiceAccount::GrantResult
   ServiceAccount::Grant(const String &path, const String &sid, DWORD rights, bool toEverythingBelow, String &detail)
   {
      detail.Empty();

      PSID binary = nullptr;
      if (!ConvertStringSidToSidW(sid.c_str(), &binary))
      {
         detail = Formatter::Format(_T("{0} is not a SID"), sid);
         return GrantFailed;
      }
      SidRelease_ release(binary);

      Permissions_ permissions;
      if (!ReadPermissions_(path, permissions, detail))
         return GrantFailed;

      // An explicit deny comes before anything this could add, so an entry
      // added now would change nothing but the look of the list: an
      // administrator's decision, left as it is and reported as a failure.
      bool explicitDeny = false;
      bool inheritedDeny = false;
      const bool covered = Covers_(permissions.dacl, binary, rights, toEverythingBelow, explicitDeny, inheritedDeny);
      if (explicitDeny)
      {
         detail = DeniedSentence_();
         return GrantFailed;
      }
      if (covered)
         return GrantAlreadyThere;

      std::vector<BYTE> buffer;
      PACL acl = BuildAcl_(permissions.dacl, binary, rights, toEverythingBelow ? (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) : 0, buffer, detail);
      if (acl == nullptr)
         return GrantFailed;

      // The protection stays what it was: a folder that inherits from its
      // parent still does, and a file written with permissions of its own
      // (ProtectedFile) keeps them. Windows hands an inheritable entry down to
      // everything below a folder as part of this call.
      const SECURITY_INFORMATION which = DACL_SECURITY_INFORMATION |
         (IsProtected_(permissions) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION);

      std::wstring writable(path.c_str());
      const DWORD result = SetNamedSecurityInfoW(&writable[0], SE_FILE_OBJECT, which, nullptr, nullptr, acl, nullptr);
      if (result != ERROR_SUCCESS)
      {
         detail = Formatter::Format(_T("its permissions could not be written (Windows error {0})"), (int) result);
         return GrantFailed;
      }

      if (inheritedDeny)
         detail = InheritedDenySentence_();
      return GrantMade;
   }

   int
   ServiceAccount::RunGrantCommand(const GrantOptions &options)
   {
      // Everything the grant says is on its own handles, which the installer, the
      // Control Panel and DBSetup read, and it says there when the database could
      // not be read (ReadConfiguredPaths_). Nothing goes to the error log or the
      // Application log. The process runs elevated, and the log folder is the
      // service's to write - Modify for NT SERVICE\hMailServer, which this verb
      // grants - so a line written there could go wherever a link the service
      // planted leads (R3, as for the live update's verbs). And the database layer
      // reports a connection this account cannot make - on SQL Server with Windows
      // authentication, an administrator with no user in the database - as the
      // server's own Critical HM5028, "Check your database settings in
      // hMailServer.INI", which it is not: until 6.3.6 every move of such an
      // installation to the virtual account wrote one to the Application log (the
      // installer smoke VM, 3 October 2026).
      Logger::ForbidWritesForThisProcess();

      if (!ProcessIsAdministrator())
      {
         SayLine_(true, _T("hMailServer.exe /GrantServiceAccess changes the permissions of the program and data folders, which takes an administrator: run it from an elevated Command Prompt. Nothing was changed."));
         return 3;
      }

      String program;
      String executable;
      if (!ProgramFolderOfThisBinary_(program, executable))
      {
         SayLine_(true, Formatter::Format(_T("{0} is not in the Bin folder of an installation, and the folders this grants are found from there, never from hMailServer.INI. Nothing was changed."), executable));
         return 2;
      }

      const String sid = ServiceSid(_T("hMailServer"));
      if (sid.IsEmpty())
      {
         SayLine_(true, _T("The service's SID could not be worked out. Nothing was changed."));
         return 1;
      }

      if (options.reportOnly)
         SayLine_(false, _T("A report only: nothing below is changed."));

      String logon;
      DWORD sidType = SERVICE_SID_TYPE_NONE;
      if (ReadService_(logon, sidType))
      {
         SayLine_(false, Formatter::Format(_T("The hMailServer service logs on as {0}."), logon));
         if (IsLocalSystemName_(logon))
         {
            SayLine_(false, _T("LocalSystem needs none of this; the grants below are made so that the service can be moved to NT SERVICE\\hMailServer (docs/ServiceAccount.md)."));
         }
         else if (!logon.StartsWith(_T("NT SERVICE\\")) && sidType == SERVICE_SID_TYPE_NONE)
         {
            SayLine_(false, _T("The service's SID is not in that account's token, so these grants do not reach it: run hMailServer.exe --service-account keep /Register from an elevated Command Prompt, which turns it on, and restart the service."));
         }
      }
      else
      {
         SayLine_(false, _T("Windows reports no hMailServer service; the grants are made all the same, for the service hMailServer.exe /Register creates."));
      }

      SayLine_(false, Formatter::Format(_T("The program folder is {0}, where this program is. The service's own identity is NT SERVICE\\hMailServer ({1})."), program, sid));

      IniFileSettings *ini = IniFileSettings::Instance();
      ini->LoadSettings();

      const Guarded_ guarded = MachineGuarded_(program);

      int made = 0;
      int there = 0;
      int failed = 0;
      int approvedNow = 0;
      int checks = 0;

      // 1. The approved list, and any approvals the command line asks for -
      //    each held to the same refusals as a folder the list already names.
      std::vector<std::pair<String, DWORD> > approved;
      String listWhy;
      const bool listTrusted = ReadApproved_(approved, listWhy);
      if (!listTrusted)
      {
         failed++;
         SayLine_(true, _T("  FAILED          ") + listWhy + _T("."));
      }

      auto recordApproval = [&](const String &requested, DWORD rights, const String &what) -> bool
      {
         const String path = Resolved_(requested);
         String why;
         PathPin_ check;
         if (RefusedLocation_(path, guarded, why) || !check.Pin(path, why))
         {
            failed++;
            SayLine_(true, Formatter::Format(_T("  REFUSED         {0} - not approved: {1}"), path, why));
            return false;
         }
         check.Release();

         if (options.reportOnly)
         {
            SayLine_(false, Formatter::Format(_T("  would approve   {0}  {1} - {2}"), RightsName_(rights), path, what));
         }
         else if (!listTrusted || !WriteApproval_(path, rights, why))
         {
            failed++;
            SayLine_(true, Formatter::Format(_T("  FAILED          {0}  {1} - not approved: {2}"), RightsName_(rights), path, listTrusted ? why : String(_T("the approved list is not trusted"))));
            return false;
         }
         else
         {
            approvedNow++;
            SayLine_(false, Formatter::Format(_T("  approved        {0}  {1} - {2}, recorded in HKEY_LOCAL_MACHINE\\{3}"), RightsName_(rights), path, what, ApprovedPathsKey()));
         }

         bool replaced = false;
         for (auto &entry : approved)
         {
            if (!SamePath_(entry.first, path))
               continue;
            if (rights == RightsModify)
               entry.second = RightsModify;
            replaced = true;
         }
         if (!replaced)
            approved.push_back(std::make_pair(path, rights));
         return true;
      };

      for (const auto &request : options.approve)
         recordApproval(request.first, request.second, _T("approved on the command line"));

      // 2. What the configuration names: the folders of hMailServer.INI
      //    [Directories], and what the database and its settings name.
      std::vector<Named_> named;
      named.push_back({ ini->GetDataDirectory(), _T("the data folder ([Directories] DataFolder)"), true, false });
      named.push_back({ ini->GetLogDirectory(), _T("the log folder ([Directories] LogFolder)"), true, false });
      named.push_back({ ini->GetTempDirectory(), _T("the temp folder ([Directories] TempFolder)"), true, false });
      if (ini->GetIsInternalDatabase() || ini->GetDatabaseType() == DatabaseSettings::TypeMSSQLCompactEdition)
         named.push_back({ ini->GetDatabaseDirectory(), _T("the database folder ([Directories] DatabaseFolder)"), true, false });
      named.push_back({ ini->GetEventDirectory(), _T("the event folder ([Directories] EventFolder)"), false, false });

      // A database this account cannot read - SQL Server with Windows
      // authentication, where the person running this is no SQL Server
      // administrator and LocalSystem is - leaves unknown what it names. Until
      // 6.3.6 that was a note, and the grant still ended in 0, so the installer,
      // DBSetup and the Control Panel moved the service without knowing whether a
      // key file it reads lies outside what it is granted. Now it is said as
      // NOT READ and ends in 7, unless an administrator confirmed (--confirm, the
      // question below); the grants themselves are made either way, from trusted
      // places alone.
      String databaseWhy;
      std::vector<String> notes;
      if (ReadConfiguredPaths_(named, databaseWhy))
         databaseWhy.Empty();
      else if (!ini->GetDatabaseSettingsExists())
      {
         // No database yet names nothing, as before.
         notes.push_back(Formatter::Format(_T("The backup folder and the certificate and key files are named in the database, and {0}, so none of them is known here; run this again once the database is set up."), databaseWhy));
         databaseWhy.Empty();
      }
      else if (databaseWhy.IsEmpty())
         databaseWhy = _T("no reason was given");

      // Each spelled as Windows resolves it - full, long - before it is compared:
      // C:\PROGRA~1\hMailServer\Data is the program's own data folder, and an
      // approval is recorded in the same form.
      for (Named_ &item : named)
      {
         if (IsDrivePath_(item.path))
            item.path = Resolved_(item.path);
      }
      for (auto &entry : approved)
      {
         if (IsDrivePath_(entry.first))
            entry.first = Resolved_(entry.first);
      }

      // 3. The plan: where the grants go, from trusted places alone.
      Plan_ plan;
      PlanGrants_(program, approved, named, guarded, plan);
      notes.insert(notes.end(), plan.notes.begin(), plan.notes.end());

      for (const auto &refusal : plan.refusedApprovals)
      {
         failed++;
         SayLine_(true, Formatter::Format(_T("  REFUSED         {0} - approved in HKEY_LOCAL_MACHINE\\{1}, and never granted: {2}"), refusal.first, ApprovedPathsKey(), refusal.second));
      }

      // A folder not approved that is not there, or runs through a link, is not
      // offered for approval: said, and left.
      std::vector<Named_> offered;
      for (const Named_ &folder : plan.notApproved)
      {
         String why;
         PathPin_ check;
         if (check.Pin(Resolved_(folder.path), why))
         {
            offered.push_back(folder);
            continue;
         }

         if (check.Missing())
            notes.push_back(Formatter::Format(_T("{0} ({1}) does not exist, so it is not granted; run this again once it does."), folder.path, folder.what));
         else
            plan.refused.push_back(std::make_pair(folder, why));
      }
      plan.notApproved = offered;

      // 4. Asked, when asked to ask: once, for everything the configuration
      //    names that the grants would not reach.
      bool confirmed = false;
      const bool unresolved = !plan.notApproved.empty() || !plan.outside.empty() || !plan.refused.empty() || !databaseWhy.IsEmpty();
      if (options.confirm && !options.reportOnly && unresolved)
      {
         if (!AskToProceed_(plan, databaseWhy))
         {
            SayWhatIsNotGranted_(plan);
            SayLine_(false, _T("The administrator answered No. Nothing was changed."));
            return 6;
         }

         confirmed = true;
         for (const Named_ &folder : plan.notApproved)
         {
            const DWORD rights = folder.writes ? RightsModify : RightsReadExecute;
            if (recordApproval(folder.path, rights, folder.what))
               plan.folders.push_back({ Normalised_(Resolved_(folder.path)), rights, folder.what });
         }
         plan.notApproved.clear();
      }

      // 5. The grants. Parents first, each once: a folder below another one
      //    granted here is then found covered by what it inherits.
      std::vector<Wanted_> folders;
      for (const Wanted_ &candidate : plan.folders)
      {
         bool duplicate = false;
         for (Wanted_ &kept : folders)
         {
            if (!SamePath_(kept.path, candidate.path))
               continue;
            duplicate = true;
            if (candidate.rights == RightsModify)
               kept.rights = RightsModify;
         }
         if (!duplicate)
            folders.push_back(candidate);
      }
      std::stable_sort(folders.begin(), folders.end(), [](const Wanted_ &first, const Wanted_ &second)
      {
         return Normalised_(first.path).GetLength() < Normalised_(second.path).GetLength();
      });

      SayLine_(false, _T("Each line is what the service's identity is granted:"));

      auto sayResult = [&](GrantResult result, DWORD rights, const String &target, const String &what, bool below, const String &detail)
      {
         const String rightsName = RightsName_(rights);
         const String everything = below ? String(_T(", and everything in it")) : String();
         if (result == GrantMade)
         {
            made++;
            SayLine_(false, Formatter::Format(_T("  granted         {0}  {1} - {2}{3}"), rightsName, target, what, everything));
         }
         else if (result == GrantAlreadyThere)
         {
            there++;
            SayLine_(false, Formatter::Format(_T("  already there   {0}  {1} - {2}{3}"), rightsName, target, what, everything));
         }
         else
         {
            failed++;
            SayLine_(true, Formatter::Format(_T("  FAILED          {0}  {1} - {2}: {3}"), rightsName, target, what, detail));
         }

         if (result != GrantFailed && !detail.IsEmpty())
            SayLine_(false, _T("                  but ") + detail + _T("."));
      };

      // A folder, held from the drive's root down while it is granted.
      auto grantFolder = [&](const String &path, DWORD rights, bool below, const String &what)
      {
         const String target = Normalised_(path);
         if (options.reportOnly)
         {
            SayLine_(false, Formatter::Format(_T("  would grant     {0}  {1} - {2}"), RightsName_(rights), target, what));
            return;
         }

         PathPin_ pin;
         String detail;
         if (!pin.Pin(target, detail))
         {
            if (pin.Missing())
            {
               notes.push_back(Formatter::Format(_T("{0} does not exist, so it is not granted ({1})."), target, what));
               return;
            }
            sayResult(GrantFailed, rights, target, what, below, detail);
            return;
         }

         const GrantResult result = Grant(target, sid, rights, below, detail);
         sayResult(result, rights, target, what, below, detail);
      };

      for (const Wanted_ &grant : folders)
      {
         if (FileUtilities::Exists(grant.path) && !FileUtilities::DirectoryExists(grant.path))
         {
            // An approval that names a file: that file, on its own.
            if (options.reportOnly)
            {
               SayLine_(false, Formatter::Format(_T("  would grant     {0}  {1} - {2}"), RightsName_(grant.rights), Normalised_(grant.path), grant.what));
               continue;
            }
            String detail;
            const GrantResult result = GrantFile_(grant.path, sid, grant.rights == RightsModify ? RightsModify : RightsRead, detail);
            sayResult(result, grant.rights, Normalised_(grant.path), grant.what, false, detail);
            continue;
         }

         grantFolder(grant.path, grant.rights, true, grant.what);
      }

      // Read on each of the program folder's own folders - Bin, DBScripts,
      // Events, Languages, WebAdmin, Addons - handed down; not on the four
      // granted Modify above. NOT on the program folder itself: the account
      // reaches the folders in it without, since every service holds "bypass
      // traverse checking", and an entry written on it would make Windows walk
      // everything below it, the message store included - after the first move,
      // a tree the service writes in, where a link of its making could carry
      // what is handed down somewhere else.
      //
      // Nor on AdminUpdate, the work folders of an administrator's install of an
      // update (Util/AdminUpdate.h): the installer copies, their logs and the
      // sign-in handoff's secret half are the administrators' alone, and the
      // service reads the two files there it needs - the last install's outcome
      // and the mark of one in progress - through entries the Updater writes on
      // each file.
      for (const String &subfolder : Subfolders_(program))
      {
         bool writable = false;
         for (const wchar_t *name : WritableDefaults_)
            writable = writable || SamePath_(subfolder, FileUtilities::Combine(program, String(name)));
         if (SamePath_(subfolder, FileUtilities::Combine(program, String(_T("AdminUpdate")))))
            continue;
         if (!writable)
            grantFolder(subfolder, RightsReadExecute, true, _T("in the program folder"));
      }

      // The files written with permissions of their own (ProtectedFile): the
      // DKIM rotation's keys and the live update's token, which inherit nothing
      // from the data folder by design, so a service moved off LocalSystem would
      // lose the keys it signs with. Each is granted to the service as well,
      // through a handle, and only while its folder is one granted Modify above
      // and has one name; its other entries, SYSTEM's and the administrators',
      // stay.
      std::vector<String> protectedFolders;
      protectedFolders.push_back(DkimRotation::KeyDirectory());
      protectedFolders.push_back(UpdateDownloader::UpdatesDirectory());
      for (const String &configuredFolder : protectedFolders)
      {
         const String folder = IsDrivePath_(configuredFolder) ? Resolved_(configuredFolder) : configuredFolder;
         if (options.reportOnly || folder.IsEmpty() || !FileUtilities::DirectoryExists(folder) || !Reaches_(folders, folder, true))
            continue;

         PathPin_ pin;
         String why;
         if (!pin.Pin(Normalised_(folder), why))
         {
            failed++;
            SayLine_(true, Formatter::Format(_T("  FAILED          Modify  {0} - the files with protected permissions in it: {1}"), folder, why));
            continue;
         }

         for (const String &file : FilesIn_(Normalised_(folder)))
         {
            String detail;
            const GrantResult result = GrantFile_(file, sid, RightsModify, detail);
            if (result == GrantMade)
            {
               made++;
               SayLine_(false, Formatter::Format(_T("  granted         Modify  {0} - a file with protected permissions"), file));
            }
            else if (result == GrantFailed)
            {
               failed++;
               SayLine_(true, Formatter::Format(_T("  FAILED          Modify  {0} - a file with protected permissions: {1}"), file, detail));
            }
         }
      }

      // The certificate and key files the configuration names inside a folder
      // granted above: Read, on the file alone, for one whose own permissions
      // keep the folder's grant from reaching it. Never on a file elsewhere -
      // the configuration could name any file on the machine.
      for (const Named_ &file : plan.filesInside)
      {
         if (options.reportOnly || !FileUtilities::Exists(file.path) || FileUtilities::DirectoryExists(file.path))
            continue;

         String detail;
         const GrantResult result = GrantFile_(file.path, sid, RightsRead, detail);
         if (result == GrantMade)
         {
            made++;
            SayLine_(false, Formatter::Format(_T("  granted         Read    {0} - {1}, whose own permissions kept the folder's grant from it"), file.path, file.what));
         }
         else if (result == GrantFailed)
         {
            failed++;
            SayLine_(true, Formatter::Format(_T("  FAILED          Read    {0} - {1}: {2}"), file.path, file.what, detail));
         }
      }

      // hMailServerApiKeys.ini, the REST API's key store in Bin, which the
      // service writes when a key is made or revoked. The one file in the
      // program folder the service may write: it holds key hashes the REST
      // listener checks, which the service could mint anyway, and no elevated
      // program takes a path, an account or a command from it. Made here,
      // empty, when it is not there yet - which the REST API reads as no keys -
      // so that the service can write into it without being able to make files
      // in Bin, where it could put a DLL beside a program an administrator runs.
      const String keyStore = RestApiServer::GetApiKeyStoreFile();
      if (!options.reportOnly && !keyStore.IsEmpty() && IsUnder_(keyStore, program))
      {
         bool keyStoreMade = false;
         if (!FileUtilities::Exists(keyStore))
         {
            HANDLE created = CreateFileW(Normalised_(keyStore).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (created != INVALID_HANDLE_VALUE)
            {
               CloseHandle(created);
               keyStoreMade = true;
            }
         }

         if (FileUtilities::Exists(keyStore))
         {
            String detail;
            const GrantResult result = GrantFile_(keyStore, sid, RightsModify, detail);
            sayResult(result, RightsModify, Normalised_(keyStore), keyStoreMade ? String(_T("the REST API's key store, made empty for it")) : String(_T("the REST API's key store, which the service writes")), false, detail);
         }
      }

      // What nobody should be able to write, and the service least: the
      // program folder, Bin, and hMailServer.INI. Said, not changed - the grant
      // removes nobody's entry - with what to run.
      std::vector<String> broad;
      broad.push_back(sid);
      broad.push_back(_T("S-1-1-0"));
      broad.push_back(_T("S-1-5-11"));
      broad.push_back(_T("S-1-5-32-545"));
      broad.push_back(_T("S-1-5-6"));
      std::vector<std::pair<String, String> > mustNotBeWritable;
      mustNotBeWritable.push_back(std::make_pair(program, String(_T("the program folder"))));
      mustNotBeWritable.push_back(std::make_pair(FileUtilities::Combine(program, _T("Bin")), String(_T("Bin, the programs an administrator runs"))));
      mustNotBeWritable.push_back(std::make_pair(IniFileSettings::GetInitializationFile(), String(_T("hMailServer.INI, which elevated commands read"))));
      for (const auto &item : mustNotBeWritable)
      {
         const String who = WritableBy_(item.first, broad);
         if (who.IsEmpty())
            continue;

         checks++;
         SayLine_(true, Formatter::Format(_T("  CHECK           {0} - {1}: {2} may write it, and the service is {2} or one of them. Take that entry away - icacls \"{0}\" /inheritance:d, then icacls \"{0}\" /remove:g *{2} - or the service can change what runs elevated."),
            item.first, item.second, who));
      }

      SayWhatIsNotGranted_(plan);

      if (!databaseWhy.IsEmpty())
      {
         SayLine_(!confirmed, Formatter::Format(_T("  NOT READ        the database: {0}. The certificate and key files, the backup folder and the PST import folder it names are not known to this account, so whether the service reaches them is not known either."), databaseWhy));
         if (!confirmed)
            SayLine_(true, _T("  Run this as an account that can open the database, or with --confirm and answer Yes, before the service is moved to NT SERVICE\\hMailServer (exit 7)."));
      }

      for (const String &note : notes)
         SayLine_(false, note);

      SayLine_(failed > 0, Formatter::Format(_T("Done: {0} granted, {1} already there, {2} failed, {3} approved, {4} to check."), made, there, failed, approvedNow, checks));

      if (failed > 0)
         return 1;
      if (!plan.refused.empty())
         return 5;
      if (!confirmed && (!plan.notApproved.empty() || !plan.outside.empty()))
         return 4;
      if (!confirmed && !databaseWhy.IsEmpty())
         return 7;
      return 0;
   }

   // ---- the service's own login on SQL Server -------------------------------

   namespace
   {
      // The service's identity as SQL Server names a Windows login made for it.
      const TCHAR *ServiceLoginName_()
      {
         return _T("NT SERVICE\\hMailServer");
      }

      // name as a bracketed T-SQL identifier, every ] in it doubled.
      String Bracketed_(const String &name)
      {
         String quoted = name;
         quoted.Replace(_T("]"), _T("]]"));
         return _T("[") + quoted + _T("]");
      }

      // text as a T-SQL Unicode literal, every ' in it doubled.
      String UnicodeLiteral_(const String &text)
      {
         String quoted = text;
         quoted.Replace(_T("'"), _T("''"));
         return _T("N'") + quoted + _T("'");
      }

      // What TryExecute and TryOpen say about a statement SQL Server refused ends
      // with the provider's own description, after the statement and the source;
      // that last line is what is worth repeating.
      String DescriptionOf_(const String &message)
      {
         String text = message;
         text.Trim();
         const int lastBreak = text.ReverseFind(_T('\n'));
         if (lastBreak >= 0)
            text = text.Mid(lastBreak + 1);
         text.Trim();
         return text.IsEmpty() ? String(_T("no reason given")) : text;
      }

      // The statements a SQL Server administrator runs to do what the verb could
      // not: each one can be run again, and leaves what is there.
      std::vector<String> DatabaseLoginStatements_(const String &database)
      {
         const String login = Bracketed_(ServiceLoginName_());
         const String literal = UnicodeLiteral_(ServiceLoginName_());

         std::vector<String> statements;
         statements.push_back(Formatter::Format(_T("IF NOT EXISTS (SELECT 1 FROM sys.server_principals WHERE name = {0}) CREATE LOGIN {1} FROM WINDOWS;"), literal, login));
         statements.push_back(Formatter::Format(_T("USE {0};"), Bracketed_(database)));
         statements.push_back(Formatter::Format(_T("IF NOT EXISTS (SELECT 1 FROM sys.database_principals WHERE name = {0}) CREATE USER {1} FOR LOGIN {1};"), literal, login));
         statements.push_back(Formatter::Format(_T("ALTER ROLE db_owner ADD MEMBER {0};"), login));
         return statements;
      }

      void SayDatabaseLoginStatements_(const String &server, const String &database)
      {
         SayLine_(true, Formatter::Format(_T("A SQL Server administrator gives it them on {0} with these statements, and the service is then moved to NT SERVICE\\hMailServer, or started again if it runs as that account:"), server));

         String oneLine;
         for (const String &statement : DatabaseLoginStatements_(database))
         {
            SayLine_(true, _T("  ") + statement);
            if (!oneLine.IsEmpty())
               oneLine += _T(" ");
            oneLine += statement;
         }

         SayLine_(true, Formatter::Format(_T("For example, from a Command Prompt running as that administrator: sqlcmd -S \"{0}\" -E -Q \"{1}\""), server, oneLine));
      }

      // The first column of the first row a query answers, as text; false, with
      // why, when it could not be read.
      bool ReadText_(std::shared_ptr<DALConnection> connection, const String &query, String &value, String &why)
      {
         value.Empty();
         std::shared_ptr<DALRecordset> rows = connection->CreateRecordset();
         if (!rows)
         {
            why = _T("no recordset could be made");
            return false;
         }

         String error;
         if (rows->TryOpen(connection, SQLCommand(query), error) != DALConnection::DALSuccess)
         {
            why = DescriptionOf_(error);
            return false;
         }

         if (!rows->IsEOF())
            value = rows->GetStringValue("value");
         return true;
      }

      // The name of the user that maps the service's login in the database the
      // connection is in - the one named after it, or any other with its SID, dbo
      // included when the login owns the database. Empty when there is none.
      bool ReadServiceUser_(std::shared_ptr<DALConnection> connection, String &user, String &why)
      {
         const String literal = UnicodeLiteral_(ServiceLoginName_());
         return ReadText_(connection, Formatter::Format(_T("select top 1 name as value from sys.database_principals where name = {0} or sid = suser_sid({0}) order by case when name = {0} then 0 else 1 end"), literal), user, why);
      }

      bool ReadIsOwner_(std::shared_ptr<DALConnection> connection, const String &user, bool &member, String &why)
      {
         String count;
         if (!ReadText_(connection, Formatter::Format(_T("select convert(nvarchar(12), count(*)) as value from sys.database_role_members m join sys.database_principals r on r.principal_id = m.role_principal_id join sys.database_principals u on u.principal_id = m.member_principal_id where r.name = N'db_owner' and u.name = {0}"), UnicodeLiteral_(user)), count, why))
            return false;
         member = !count.IsEmpty() && count != _T("0");
         return true;
      }

      String ComputerName_(COMPUTER_NAME_FORMAT format)
      {
         wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 64] = {};
         DWORD size = MAX_COMPUTERNAME_LENGTH + 63;
         if (!GetComputerNameExW(format, buffer, &size))
            return String();
         return String(buffer);
      }
   }

   int
   ServiceAccount::RunDatabaseLoginCommand()
   {
      // Everything this says is on its own handles, which the installer reads into
      // its log. Nothing goes to the error log or the Application log: a connection
      // the account running it cannot make is reported there by the database layer
      // as the server's own failure to reach its database (HM5028, "Check your
      // database settings in hMailServer.ini"), which it is not; and the log folder
      // is the service's to write, which an elevated process does not write into.
      Logger::ForbidWritesForThisProcess();

      IniFileSettings *ini = IniFileSettings::Instance();
      ini->LoadSettings();

      if (!ini->GetDatabaseSettingsExists())
      {
         SayLine_(true, _T("hMailServer.INI names no database, so there is no database to give the service's account a login on. Nothing was changed."));
         return 2;
      }

      const String server = ini->GetDatabaseServer();
      const String database = ini->GetDatabaseName();

      if (ini->GetDatabaseType() != DatabaseSettings::TypeMSSQLServer)
      {
         SayLine_(false, _T("The database is not on SQL Server: hMailServer signs in to it with the user and password in hMailServer.INI, whatever account the service runs as. Nothing to do."));
         return 0;
      }

      if (!ini->GetUsername().IsEmpty())
      {
         SayLine_(false, Formatter::Format(_T("hMailServer signs in to SQL Server {0} as {1}, with a password, whatever account the service runs as. Nothing to do."), server, ini->GetUsername()));
         return 0;
      }

      const String self = ProcessAccountName();
      const String elevated = ProcessIsAdministrator() ? String() : String(_T(" This was not run from an elevated prompt: an administrator's Windows account that is a SQL Server administrator through the Administrators group is one only when elevated."));

      std::shared_ptr<DALConnection> connection = DALConnectionFactory::CreateConnection(SchemaUpgrader::SettingsFromIni());
      String error;
      if (!connection || connection->Connect(error) != DALConnection::Connected)
      {
         SayLine_(true, Formatter::Format(_T("The database {0} on SQL Server {1} could not be opened as {2}: {3}{4}"), database, server, self, DescriptionOf_(error), elevated));
         SayLine_(true, _T("So NT SERVICE\\hMailServer, which the service runs as once it is moved, was given nothing there."));
         SayDatabaseLoginStatements_(server, database);
         return 2;
      }

      // Where SQL Server runs. The service's virtual account is NT SERVICE\hMailServer
      // only to a SQL Server on this computer; to one on another it is this
      // computer's own account, DOMAIN\COMPUTER$, as LocalSystem is, and a login of
      // that name could not even be made there.
      String machine;
      String physical;
      String why;
      ReadText_(connection, _T("select convert(nvarchar(128), serverproperty('MachineName')) as value"), machine, why);
      ReadText_(connection, _T("select convert(nvarchar(128), serverproperty('ComputerNamePhysicalNetBIOS')) as value"), physical, why);

      const String here = ComputerName_(ComputerNameNetBIOS);
      const String herePhysical = ComputerName_(ComputerNamePhysicalNetBIOS);
      auto isHere = [&here, &herePhysical](const String &name)
      {
         return !name.IsEmpty() && (name.CompareNoCase(here) == 0 || name.CompareNoCase(herePhysical) == 0);
      };

      if ((!machine.IsEmpty() || !physical.IsEmpty()) && !isHere(machine) && !isHere(physical))
      {
         connection->Disconnect();
         SayLine_(false, Formatter::Format(_T("SQL Server {0} runs on {1}, not on this computer ({2}): there NT SERVICE\\hMailServer signs in as this computer's own account, as LocalSystem does, and needs no login of its own. Nothing to do."),
            server, machine.IsEmpty() ? physical : machine, here));
         return 0;
      }

      SayLine_(false, Formatter::Format(_T("SQL Server {0}, on this computer, as {1}; the database {2}:"), server, self, database));

      std::vector<String> refusals;

      // The login. Looked for twice, because a SQL Server login that is not a
      // server administrator sees only some of the others in sys.server_principals.
      String login;
      if (!ReadText_(connection, Formatter::Format(_T("select case when suser_id({0}) is not null or exists (select 1 from sys.server_principals where name = {0}) then N'1' else N'0' end as value"), UnicodeLiteral_(ServiceLoginName_())), login, why))
         refusals.push_back(Formatter::Format(_T("whether the login {0} exists could not be read: {1}"), ServiceLoginName_(), why));

      if (login == _T("1"))
         SayLine_(false, Formatter::Format(_T("  the login {0} is there already"), ServiceLoginName_()));
      else if (login == _T("0"))
      {
         if (connection->TryExecute(SQLCommand(Formatter::Format(_T("create login {0} from windows"), Bracketed_(ServiceLoginName_()))), error) == DALConnection::DALSuccess)
            SayLine_(false, Formatter::Format(_T("  made the login {0} (FROM WINDOWS)"), ServiceLoginName_()));
         else
            refusals.push_back(Formatter::Format(_T("the login {0} could not be made: {1}"), ServiceLoginName_(), DescriptionOf_(error)));
      }

      // A trigger the database has of its own on its DDL (sys.triggers, parent
      // class 0) runs as the account whose statement fires it, with that account's
      // rights on SQL Server: a CREATE USER or an ALTER ROLE made here, as an
      // administrator who is often sysadmin, would run whatever anybody who was
      // db_owner of this database - an earlier server's login, another program's -
      // put in one, server-wide. So with one there, nothing is changed in the
      // database, and a SQL Server administrator, who reads them, runs the
      // statements. Disabled ones do not fire.
      String noChange;
      String triggers;
      if (!ReadText_(connection, _T("select convert(nvarchar(12), count(*)) as value from sys.triggers where parent_class = 0 and is_disabled = 0"), triggers, why))
         noChange = Formatter::Format(_T("whether {0} has DDL triggers of its own could not be read ({1}), and one would run with this account's rights on SQL Server"), database, why);
      else if (!triggers.IsEmpty() && triggers != _T("0"))
         noChange = Formatter::Format(_T("{0} has DDL triggers of its own ({1}, in sys.triggers), which would run with this account's rights on SQL Server; read them before the statements below are run"), database, triggers);

      // Its user in the database, and that user in db_owner. A login that owns the
      // database is its dbo already.
      String user;
      if (!ReadServiceUser_(connection, user, why))
         refusals.push_back(Formatter::Format(_T("whether the database {0} has a user for {1} could not be read: {2}"), database, ServiceLoginName_(), why));
      else if (!user.IsEmpty())
         SayLine_(false, Formatter::Format(_T("  the user {0} is there already"), user));
      else if (!noChange.IsEmpty())
         refusals.push_back(Formatter::Format(_T("the user {0} was not made in {1}: {2}"), ServiceLoginName_(), database, noChange));
      else if (connection->TryExecute(SQLCommand(Formatter::Format(_T("create user {0} for login {0}"), Bracketed_(ServiceLoginName_()))), error) == DALConnection::DALSuccess)
      {
         SayLine_(false, Formatter::Format(_T("  made the user {0} in {1}"), ServiceLoginName_(), database));
         ReadServiceUser_(connection, user, why);
      }
      else
         refusals.push_back(Formatter::Format(_T("the user {0} could not be made in {1}: {2}"), ServiceLoginName_(), database, DescriptionOf_(error)));

      bool owner = false;
      if (!user.IsEmpty())
      {
         if (user.CompareNoCase(_T("dbo")) == 0)
         {
            owner = true;
            SayLine_(false, Formatter::Format(_T("  {0} owns {1}"), ServiceLoginName_(), database));
         }
         else if (!ReadIsOwner_(connection, user, owner, why))
            refusals.push_back(Formatter::Format(_T("whether {0} is in db_owner could not be read: {1}"), user, why));
         else if (owner)
            SayLine_(false, Formatter::Format(_T("  {0} is in db_owner already"), user));
         else if (!noChange.IsEmpty())
            refusals.push_back(Formatter::Format(_T("{0} was not added to db_owner: {1}"), user, noChange));
         else if (connection->TryExecute(SQLCommand(Formatter::Format(_T("alter role db_owner add member {0}"), Bracketed_(user))), error) == DALConnection::DALSuccess)
         {
            ReadIsOwner_(connection, user, owner, why);
            if (owner)
               SayLine_(false, Formatter::Format(_T("  added {0} to db_owner"), user));
            else
               refusals.push_back(Formatter::Format(_T("{0} was added to db_owner, and is not in it"), user));
         }
         else
            refusals.push_back(Formatter::Format(_T("{0} could not be added to db_owner: {1}"), user, DescriptionOf_(error)));
      }

      connection->Disconnect();

      if (refusals.empty() && !user.IsEmpty() && owner)
      {
         SayLine_(false, Formatter::Format(_T("NT SERVICE\\hMailServer can open {0}: the service opens its database as that account once it runs as it."), database));
         return 0;
      }

      for (const String &refusal : refusals)
         SayLine_(true, _T("  ") + refusal + _T("."));
      SayLine_(true, Formatter::Format(_T("NT SERVICE\\hMailServer cannot open {0} on SQL Server {1} yet, and {2} could not give it what it needs.{3}"), database, server, self, elevated));
      SayDatabaseLoginStatements_(server, database);
      return 1;
   }

   // ---- the tester --------------------------------------------------------

   namespace
   {
      void FailGrant_(const String &sentence)
      {
         throw std::logic_error(std::string(AnsiString(_T("ServiceAccount: ") + sentence).c_str()));
      }

      // The explicit (not inherited) allow entries for sid on path, as
      // mask and flags.
      std::vector<std::pair<DWORD, BYTE>> OwnEntries_(const String &path, const String &sid)
      {
         std::vector<std::pair<DWORD, BYTE>> entries;

         PSID binary = nullptr;
         if (!ConvertStringSidToSidW(sid.c_str(), &binary))
            return entries;

         Permissions_ permissions;
         String error;
         ACL_SIZE_INFORMATION size;
         if (ReadPermissions_(path, permissions, error) && permissions.dacl != nullptr &&
             GetAclInformation(permissions.dacl, &size, sizeof(size), AclSizeInformation))
         {
            for (DWORD index = 0; index < size.AceCount; index++)
            {
               LPVOID entry = nullptr;
               if (!GetAce(permissions.dacl, index, &entry) || entry == nullptr)
                  continue;
               const ACE_HEADER *header = (const ACE_HEADER *) entry;
               if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERITED_ACE) != 0)
                  continue;
               const ACCESS_ALLOWED_ACE *allow = (const ACCESS_ALLOWED_ACE *) entry;
               if (EqualSid((PSID) &allow->SidStart, binary))
                  entries.push_back(std::make_pair(allow->Mask, header->AceFlags));
            }
         }

         LocalFree(binary);
         return entries;
      }

      bool ProtectedPath_(const String &path)
      {
         Permissions_ permissions;
         String error;
         return ReadPermissions_(path, permissions, error) && IsProtected_(permissions);
      }

      // Which folders a grant may reach, on a machine made up for the purpose:
      // no folder here needs to exist.
      void TestLocations_()
      {
         Guarded_ guarded;
         guarded.program = _T("C:\\Program Files\\hMailServer");
         guarded.nowhereNear.push_back(_T("C:\\Windows"));
         guarded.nowhereNear.push_back(_T("C:\\Program Files"));
         guarded.nowhereNear.push_back(_T("C:\\Program Files (x86)"));
         guarded.nowhereNear.push_back(_T("C:\\Users"));
         guarded.nowhereNear.push_back(_T("C:\\ProgramData\\Microsoft"));
         guarded.notAbove.push_back(_T("C:\\ProgramData"));

         const wchar_t *const allowed[] =
         {
            L"C:\\Program Files\\hMailServer\\Data",
            L"C:\\Program Files\\hMailServer\\Data\\Certificates",
            L"C:\\Program Files\\hMailServer\\Logs",
            L"C:\\Program Files\\hMailServer\\Database\\",
            L"C:\\ProgramData\\hMailServer\\Data",
            L"D:\\Mail",
            L"D:\\Mail\\Backups",
            L"e:\\archive",
         };
         for (const wchar_t *path : allowed)
         {
            String why;
            if (RefusedLocation_(String(path), guarded, why))
               FailGrant_(Formatter::Format(_T("{0} is refused ({1}), and a grant may reach it."), String(path), why));
         }

         // The paths a service that can write the configuration would name to
         // be given Modify where an administrator's programs are - the first
         // three are [Directories] DataFolder=Bin, =. and =Data\..\Bin with the
         // program folder under them - and the rest of what is guarded.
         const wchar_t *const refused[] =
         {
            L"C:\\Program Files\\hMailServer\\Bin",
            L"C:\\Program Files\\hMailServer",
            L"C:\\Program Files\\hMailServer\\Data\\..\\Bin",
            L"C:\\Program Files\\hMailServer\\.",
            L"C:\\Program Files\\hMailServer\\Events",
            L"C:\\Program Files\\hMailServer\\Bin\\Plugins",
            L"C:\\Program Files",
            L"C:\\Program Files\\Some Other Program",
            L"C:\\Program Files (x86)\\Some Other Program",
            L"C:\\Windows\\System32",
            L"C:\\Windows",
            L"C:\\",
            L"C:",
            L"D:\\",
            L"C:\\Users",
            L"C:\\Users\\Administrator\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup",
            L"C:\\ProgramData",
            L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\StartUp",
            L"\\\\server\\share\\mail",
            L"\\\\?\\C:\\Mail",
            L"\\\\.\\C:\\Mail",
            L"Data",
            L"..\\Data",
            L"D:\\Mail:stream",
            L"",
         };
         for (const wchar_t *path : refused)
         {
            String why;
            if (!RefusedLocation_(String(path), guarded, why) || why.IsEmpty())
               FailGrant_(Formatter::Format(_T("\"{0}\" is not refused, and a grant must never reach it."), String(path)));
         }

         // The plan: a [Directories] value pointing at Bin is REFUSED, never a
         // grant; an unapproved backup folder is NOT APPROVED; the same folder
         // approved is a grant; a key file outside is OUTSIDE and one inside
         // the data folder is read on the file alone; an approval of the Windows
         // folder is refused although it is listed.
         std::vector<std::pair<String, DWORD> > approved;
         approved.push_back(std::make_pair(String(_T("D:\\Approved")), (DWORD) ServiceAccount::RightsModify));
         approved.push_back(std::make_pair(String(_T("C:\\Windows\\Temp")), (DWORD) ServiceAccount::RightsModify));

         std::vector<Named_> named;
         named.push_back({ _T("C:\\Program Files\\hMailServer\\Bin"), _T("the data folder ([Directories] DataFolder)"), true, false });
         named.push_back({ _T("D:\\Backups"), _T("the backup folder"), true, false });
         named.push_back({ _T("D:\\Approved\\Archive"), _T("the archive folder (ArchiveDir)"), true, false });
         named.push_back({ _T("C:\\Certificates\\mail.key"), _T("a private key"), false, true });
         named.push_back({ _T("C:\\Program Files\\hMailServer\\Data\\Certificates\\mail.key"), _T("another private key"), false, true });
         named.push_back({ _T("C:\\Program Files\\hMailServer\\Events"), _T("the event folder ([Directories] EventFolder)"), false, false });

         Plan_ plan;
         PlanGrants_(guarded.program, approved, named, guarded, plan);

         for (const Wanted_ &folder : plan.folders)
         {
            if (SamePath_(folder.path, _T("C:\\Program Files\\hMailServer\\Bin")) || SamePath_(folder.path, _T("C:\\Windows\\Temp")))
               FailGrant_(Formatter::Format(_T("The plan grants {0}."), folder.path));
         }
         if (plan.folders.size() != 5)
            FailGrant_(Formatter::Format(_T("The plan grants {0} folders; the four of the program folder and the one approval were expected."), (int) plan.folders.size()));
         if (plan.refused.size() != 1 || !SamePath_(plan.refused[0].first.path, _T("C:\\Program Files\\hMailServer\\Bin")))
            FailGrant_(_T("The data folder pointed at Bin is not the one refusal of the plan."));
         if (plan.refusedApprovals.size() != 1 || !SamePath_(plan.refusedApprovals[0].first, _T("C:\\Windows\\Temp")))
            FailGrant_(_T("An approval of a folder in the Windows folder is not refused."));
         if (plan.notApproved.size() != 1 || !SamePath_(plan.notApproved[0].path, _T("D:\\Backups")))
            FailGrant_(_T("The unapproved backup folder is not the one folder the plan lists as not approved."));
         if (plan.outside.size() != 1 || !SamePath_(plan.outside[0].path, _T("C:\\Certificates\\mail.key")))
            FailGrant_(_T("The key file outside every granted folder is not the one file the plan lists as outside."));
         if (plan.filesInside.size() != 1 || !SamePath_(plan.filesInside[0].path, _T("C:\\Program Files\\hMailServer\\Data\\Certificates\\mail.key")))
            FailGrant_(_T("The key file in the data folder is not the one file the plan reads on its own."));

         // A folder approved for Read does not reach a folder the service
         // writes in it.
         std::vector<std::pair<String, DWORD> > readOnly;
         readOnly.push_back(std::make_pair(String(_T("D:\\ReadOnly")), (DWORD) ServiceAccount::RightsReadExecute));
         std::vector<Named_> writesThere;
         writesThere.push_back({ _T("D:\\ReadOnly\\Archive"), _T("the archive folder (ArchiveDir)"), true, false });
         Plan_ second;
         PlanGrants_(guarded.program, readOnly, writesThere, guarded, second);
         if (second.notApproved.size() != 1)
            FailGrant_(_T("A folder the service writes, under a folder approved for Read only, is not listed as not approved."));
      }

      // The logon /Register passes for each --service-account.
      void TestLogons_()
      {
         ServiceManager::Logon logon;
         String refusal;

         if (!ServiceManager::LogonFor(ServiceManager::AccountVirtual, _T("hMailServer"), String(), logon, refusal) ||
             !logon.change || logon.name != _T("NT SERVICE\\hMailServer") || logon.emptyPassword)
            FailGrant_(_T("--service-account virtual is not NT SERVICE\\hMailServer with a NULL password."));

         if (!ServiceManager::LogonFor(ServiceManager::AccountLocalSystem, _T("hMailServer"), String(), logon, refusal) ||
             !logon.change || logon.name != _T("LocalSystem") || !logon.emptyPassword)
            FailGrant_(_T("--service-account localsystem is not LocalSystem with an empty password."));

         if (!ServiceManager::LogonFor(ServiceManager::AccountKeep, _T("hMailServer"), String(), logon, refusal) || logon.change)
            FailGrant_(_T("--service-account keep, and no --service-account at all, changes the account."));

         if (!ServiceManager::LogonFor(ServiceManager::AccountManaged, _T("hMailServer"), _T("CONTOSO\\hmail$"), logon, refusal) ||
             !logon.change || logon.name != _T("CONTOSO\\hmail$") || logon.emptyPassword)
            FailGrant_(_T("A group managed service account is not passed with a NULL password."));

         // What /Register must never take: an account with a password, and
         // LocalSystem or anything else under another name.
         const wchar_t *const refused[] =
         {
            L"CONTOSO\\mailuser",
            L"LocalSystem",
            L".\\LocalSystem",
            L"NT AUTHORITY\\SYSTEM",
            L"NT AUTHORITY\\NetworkService",
            L"hmail$",
            L"\\hmail$",
            L"CONTOSO\\$",
            L"CONTOSO\\a\\b$",
            L"CONTOSO\\hm ail$",
            L"",
         };
         for (const wchar_t *name : refused)
         {
            if (ServiceManager::LogonFor(ServiceManager::AccountManaged, _T("hMailServer"), String(name), logon, refusal) || refusal.IsEmpty())
               FailGrant_(Formatter::Format(_T("--service-account \"{0}\" is taken, and only a managed service account's name may be."), String(name)));
         }
      }
   }

   void
   ServiceAccountTester::Test()
   {
      // The SID as Windows derives it, against one Windows publishes - the
      // TrustedInstaller service's, which every ACL under System32 names - in
      // either case, and hMailServer's own as docs/ServiceAccount.md gives it.
      const String trustedInstaller = _T("S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464");
      if (ServiceAccount::ServiceSid(_T("TrustedInstaller")) != trustedInstaller)
         FailGrant_(_T("ServiceSid(\"TrustedInstaller\") is not the SID Windows gives that service: ") + ServiceAccount::ServiceSid(_T("TrustedInstaller")));
      if (ServiceAccount::ServiceSid(_T("trustedinstaller")) != trustedInstaller)
         FailGrant_(_T("ServiceSid does not upper-case the name first, as Windows does."));
      if (ServiceAccount::ServiceSid(_T("hMailServer")) != _T("S-1-5-80-3504853920-2627644491-1723263464-2546768561-1700046809"))
         FailGrant_(_T("ServiceSid(\"hMailServer\") is not the SID docs/ServiceAccount.md gives: ") + ServiceAccount::ServiceSid(_T("hMailServer")));

      // This process's own account, as the setup checklist names it.
      if (ServiceAccount::ProcessAccountName().IsEmpty())
         FailGrant_(_T("The account this process runs as could not be named."));
      if (ServiceAccount::ProcessIsMostPrivileged() && !ServiceAccount::ProcessIsAdministrator())
         FailGrant_(_T("This process runs as LocalSystem and is not reported as an administrator."));

      TestLocations_();
      TestLogons_();

      // A SID of the tester's own, never a real service's, on a directory of
      // its own under the temp folder: nothing here touches the server's.
      const String sid = ServiceAccount::ServiceSid(_T("hMailServerGrantTest"));
      const String other = ServiceAccount::ServiceSid(_T("hMailServerNeverGranted"));
      const String backupOperators = _T("S-1-5-32-551");
      const String directory = IniFileSettings::Instance()->GetTempDirectory() + FileUtilities::PathSeparator + _T("ServiceAccountTest-") + GUIDCreator::GetGUID();
      if (!FileUtilities::CreateDirectory(directory))
         FailGrant_(_T("The tester's directory could not be created under the temp folder."));

      try
      {
         String detail;

         // The administrator's own entry, before the grant: Backup Operators may
         // read the folder. The grant must leave it.
         if (ServiceAccount::Grant(directory, backupOperators, ServiceAccount::RightsReadExecute, true, detail) != ServiceAccount::GrantMade)
            FailGrant_(_T("The administrator's entry could not be made on the tester's directory: ") + detail);

         // A file already there before the grant, which has to inherit it.
         const String before = FileUtilities::Combine(directory, _T("before.txt"));
         if (!FileUtilities::WriteToFile(before, AnsiString("before")))
            FailGrant_(_T("The tester's file could not be written."));

         if (ServiceAccount::HasGrant(directory, sid, ServiceAccount::RightsModify, true, detail))
            FailGrant_(_T("The directory reports a grant before one was made."));

         const ServiceAccount::GrantResult first = ServiceAccount::Grant(directory, sid, ServiceAccount::RightsModify, true, detail);
         if (first != ServiceAccount::GrantMade)
            FailGrant_(_T("The grant on the tester's directory was not made: ") + detail);

         // Again: already there, and nothing written.
         const ServiceAccount::GrantResult second = ServiceAccount::Grant(directory, sid, ServiceAccount::RightsModify, true, detail);
         if (second != ServiceAccount::GrantAlreadyThere)
            FailGrant_(_T("A second grant on the same directory did not find the first one."));

         // Read is less than Modify, so it is there already too.
         if (ServiceAccount::Grant(directory, sid, ServiceAccount::RightsReadExecute, true, detail) != ServiceAccount::GrantAlreadyThere)
            FailGrant_(_T("A Read grant on a directory granted Modify was not found covered."));

         // The DACL: one explicit allow entry for the SID, Modify, handed down to
         // folders and files, not inherit-only; the administrator's entry still
         // there, untouched; and none for a SID nobody granted.
         std::vector<std::pair<DWORD, BYTE>> own = OwnEntries_(directory, sid);
         if (own.size() != 1)
            FailGrant_(Formatter::Format(_T("The directory has {0} explicit entries for the service's SID after two grants; one was expected."), (int) own.size()));
         if (own[0].first != ServiceAccount::RightsModify)
         {
            String mask;
            mask.Format(_T("0x%08X"), (unsigned int) own[0].first);
            FailGrant_(_T("The service's entry grants ") + mask + _T(" and not Modify (0x001301BF)."));
         }
         if ((own[0].second & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) != (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) ||
             (own[0].second & (INHERIT_ONLY_ACE | NO_PROPAGATE_INHERIT_ACE)) != 0)
            FailGrant_(_T("The service's entry on the directory is not handed down to both the folders and the files below it."));

         std::vector<std::pair<DWORD, BYTE>> administrators = OwnEntries_(directory, backupOperators);
         if (administrators.size() != 1 || administrators[0].first != ServiceAccount::RightsReadExecute)
            FailGrant_(_T("The administrator's own entry on the directory did not survive the grant unchanged."));

         if (!OwnEntries_(directory, other).empty() || ServiceAccount::HasGrant(directory, other, ServiceAccount::RightsReadExecute, true, detail))
            FailGrant_(_T("A SID nobody granted is reported as granted."));

         // The file that was there before, and one made after: both inherit.
         if (!ServiceAccount::HasGrant(before, sid, ServiceAccount::RightsModify, false, detail))
            FailGrant_(_T("The file already in the directory did not inherit the grant."));
         const String after = FileUtilities::Combine(directory, _T("after.txt"));
         if (!FileUtilities::WriteToFile(after, AnsiString("after")) || !ServiceAccount::HasGrant(after, sid, ServiceAccount::RightsModify, false, detail))
            FailGrant_(_T("A file made after the grant did not inherit it."));

         // A file with permissions of its own, as ProtectedFile writes a DKIM key:
         // it inherits nothing, so the directory's grant does not reach it, and
         // granted on its own - through a handle, as the verb grants it - it
         // keeps its protection and its other entries.
         const String secret = FileUtilities::Combine(directory, _T("protected.txt"));
         String error;
         if (!ProtectedFile::Write(secret, AnsiString("secret"), error))
            FailGrant_(_T("The tester's protected file could not be written: ") + error);
         if (!ProtectedPath_(secret))
            FailGrant_(_T("ProtectedFile wrote a file whose permissions are not protected."));
         if (ServiceAccount::HasGrant(secret, sid, ServiceAccount::RightsModify, false, detail))
            FailGrant_(_T("A protected file reports the directory's grant, which it cannot inherit."));

         const String system = _T("S-1-5-18");
         const size_t systemEntries = OwnEntries_(secret, system).size();
         if (GrantFile_(secret, sid, ServiceAccount::RightsModify, detail) != ServiceAccount::GrantMade)
            FailGrant_(_T("The grant on the protected file was not made: ") + detail);
         if (!ProtectedPath_(secret))
            FailGrant_(_T("The grant took the protected file's protection away."));
         if (OwnEntries_(secret, system).size() != systemEntries || systemEntries == 0)
            FailGrant_(_T("The grant changed SYSTEM's entry on the protected file."));
         if (!ServiceAccount::HasGrant(secret, sid, ServiceAccount::RightsModify, false, detail))
            FailGrant_(_T("The protected file does not report the grant just made."));
         if (GrantFile_(secret, sid, ServiceAccount::RightsModify, detail) != ServiceAccount::GrantAlreadyThere)
            FailGrant_(_T("A second grant on the protected file did not find the first one."));

         // A file with a second name - a hard link a service could make, in a
         // folder it writes, to a file it wants - is refused, and keeps its
         // permissions.
         const String target = FileUtilities::Combine(directory, _T("target.txt"));
         const String link = FileUtilities::Combine(directory, _T("link.txt"));
         if (!ProtectedFile::Write(target, AnsiString("target"), error))
            FailGrant_(_T("The tester's link target could not be written: ") + error);
         if (CreateHardLinkW(link.c_str(), target.c_str(), nullptr))
         {
            if (GrantFile_(link, sid, ServiceAccount::RightsRead, detail) != ServiceAccount::GrantFailed || detail.IsEmpty())
               FailGrant_(_T("A file with two names was granted through one of them."));
            if (ServiceAccount::HasGrant(target, sid, ServiceAccount::RightsRead, false, detail))
               FailGrant_(_T("The refused grant on a hard link reached the file it names."));
         }

         // A folder reached through a symbolic link is refused by the pin that
         // holds a path while it is granted. Made only where this process may
         // make one; not every machine lets it, and then this case is skipped.
         const String real = FileUtilities::Combine(directory, _T("real"));
         const String through = FileUtilities::Combine(directory, _T("through"));
         if (FileUtilities::CreateDirectory(real) && CreateSymbolicLinkW(through.c_str(), real.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2))
         {
            PathPin_ pin;
            String why;
            if (pin.Pin(FileUtilities::Combine(through, _T("inside")), why) || why.IsEmpty() || pin.Missing())
               FailGrant_(_T("A path through a symbolic link is not refused by the pin."));
            pin.Release();
            RemoveDirectoryW(through.c_str());
         }

         // The pin holds a plain path, and a held folder cannot be removed.
         {
            PathPin_ pin;
            String why;
            if (!pin.Pin(real, why))
               FailGrant_(_T("A plain folder could not be held: ") + why);
            if (RemoveDirectoryW(real.c_str()))
               FailGrant_(_T("A folder the pin holds could be removed from under it."));
         }

         // A path that is not there fails with a sentence, and changes nothing.
         const String missing = FileUtilities::Combine(directory, _T("missing"));
         if (ServiceAccount::Grant(missing, sid, ServiceAccount::RightsModify, true, detail) != ServiceAccount::GrantFailed || detail.IsEmpty())
            FailGrant_(_T("A grant on a path that does not exist did not fail with a reason."));
      }
      catch (...)
      {
         FileUtilities::DeleteDirectory(directory, true);
         throw;
      }

      FileUtilities::DeleteDirectory(directory, true);
   }

#endif
}
