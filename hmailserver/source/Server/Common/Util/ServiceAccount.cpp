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
#include <winioctl.h>
#include <knownfolders.h>
#include <openssl/evp.h>

#include "AdminUpdateChecks.h"
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

      struct Guarded_;
      bool RefusedLocation_(const String &path, const Guarded_ &guarded, String &why);

      // Holds every folder from the drive's root down to a path open, without
      // sharing delete, so that none of them can be renamed, deleted or swapped
      // for a link while a grant is written through the path, and refuses a path
      // that is not on a fixed local drive. It holds the path as the store
      // commands hold the data folder (HeldFolder, ProtectedFile.h): each folder
      // opened relative to the one above it, never through a link unchecked. A
      // junction, a mount point or a symbolic link on the way is gone through
      // only when the administrators alone control it (OnlyAdministratorsControl)
      // - one an administrator made to keep the data, the logs or a folder of
      // the program on another drive - and where it leads is then held in the
      // same way from that volume's root, must be on a fixed local drive and,
      // when guarded is given, must be a folder a grant may reach
      // (RefusedLocation_): nothing but an administrator can have chosen it, and
      // it is held to the same refusals as a folder named directly. Any other
      // link is refused, naming who may change it or who owns it. Until 6.3.6
      // every link on the way was refused, and so was the grant of an
      // installation whose data folder an administrator had moved to another
      // drive behind a junction - which failed its upgrade. Released when it
      // goes.
      class PathPin_
      {
      public:
         PathPin_() : missing_(false) {}
         ~PathPin_() { Release(); }

         bool Pin(const String &path, String &why, const Guarded_ *guarded = nullptr)
         {
            Release();
            missing_ = false;
            through_.Empty();
            throughByVolume_.Empty();

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

            String error;
            if (!held_.Hold(target, false, error))
            {
               missing_ = held_.Missing();
               why = error;
               // The sentences HeldFolder ends with a full stop; the grant's
               // lines add their own.
               while (why.Right(1) == _T("."))
                  why = why.Left(why.GetLength() - 1);
               Release();
               return false;
            }

            if (held_.WentThroughLink())
            {
               const String reached = held_.Reached();
               const String byVolume = held_.ReachedByVolume();
               const int close = byVolume.Find(_T("}\\"));
               const String volume = byVolume.Left(11).CompareNoCase(_T("\\\\?\\Volume{")) == 0 && close > 11 ? byVolume.Left(close + 2) : String();
               const String named = reached.IsEmpty() ? byVolume : reached;

               if (volume.IsEmpty() || GetDriveTypeW(volume.c_str()) != DRIVE_FIXED)
               {
                  why = Formatter::Format(_T("{0} leads through a link to {1}, which is not on a fixed local drive, and only those are granted"), target, named);
                  Release();
                  return false;
               }

               // A folder of a volume no drive letter names - one mounted on a
               // folder alone - is none of the guarded folders, which are all
               // named from a drive letter.
               String refusal;
               if (guarded != nullptr && !reached.IsEmpty() && RefusedLocation_(reached, *guarded, refusal))
               {
                  why = Formatter::Format(_T("{0} leads through a link to {1}, which is never granted: {2}"), target, reached, refusal);
                  Release();
                  return false;
               }

               // Without a trailing separator: where a mount point leads is a
               // volume's root, which Windows names with one (C:\...\Logs\).
               through_ = Normalised_(named);
               throughByVolume_ = byVolume;
            }

            return true;
         }

         // After a failed Pin: whether it failed because the path is not there.
         bool Missing() const { return missing_; }

         // After a Pin: where the path leads when it goes through a link the
         // administrators alone control, as Windows names it with a drive letter
         // where it can; empty when it goes through none.
         String Through() const { return through_; }

         // The path to write a grant through: the one asked for, or - through a
         // link - the folder it leads to, by its volume's name. A permission
         // written by a path is handed down from the folder that path names
         // above it, and the folder above the link is not the one above where it
         // leads; by its volume's name the path has no link in it at all.
         String GrantPath(const String &asked) const { return throughByVolume_.IsEmpty() ? Normalised_(asked) : throughByVolume_; }

         void Release()
         {
            held_.Release();
         }

      private:
         PathPin_(const PathPin_ &);
         PathPin_ &operator=(const PathPin_ &);

         HeldFolder held_;
         bool missing_;
         String through_;
         String throughByVolume_;
      };

      // Grants sid the rights on one file, through a handle opened without
      // following links, with every folder above it held (PathPin_), and only
      // when it is a file with one name: a hard link elsewhere would carry the
      // grant to wherever else the file is. The protection of its permissions
      // stays what it was. guarded: what a link on the way may not lead to
      // (PathPin_).
      ServiceAccount::GrantResult GrantFile_(const String &file, const String &sidText, DWORD rights, String &detail, const Guarded_ *guarded = nullptr)
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
         if (folder.IsEmpty() || !pin.Pin(folder, detail, guarded))
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

      // The immediate subfolders of a folder, each with whether it is a junction,
      // a mount point or a symbolic link to a folder: somebody else's folder,
      // reached through this one, gone through only when the administrators
      // alone control it (PathPin_).
      std::vector<std::pair<String, bool> > Subfolders_(const String &directory)
      {
         std::vector<std::pair<String, bool> > found;
         WIN32_FIND_DATAW data;
         HANDLE search = FindFirstFileExW(FileUtilities::Combine(directory, _T("*")).c_str(), FindExInfoBasic, &data, FindExSearchLimitToDirectories, nullptr, 0);
         if (search == INVALID_HANDLE_VALUE)
            return found;

         do
         {
            const std::wstring name = data.cFileName;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
               continue;
            if (name == L"." || name == L"..")
               continue;
            found.push_back(std::make_pair(FileUtilities::Combine(directory, String(name.c_str())), (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0));
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
      // add, change or delete files in a folder, change or delete a file, or
      // change who may (AdminUpdateChecks::WriteRights, the rule an
      // administrator's install of an update holds the program's folders to).
      // The name of the first that may, or empty. What the grant checks the
      // program folder and hMailServer.INI against, and says - it removes
      // nobody's entry. Until 6.3.7 it did not count deleting what is in a
      // folder, or changing a file's attributes.
      String WritableBy_(const String &path, const std::vector<String> &sids)
      {
         Permissions_ permissions;
         String error;
         if (!ReadPermissions_(path, permissions, error) || permissions.dacl == nullptr)
            return String();

         ACL_SIZE_INFORMATION size;
         if (!GetAclInformation(permissions.dacl, &size, sizeof(size), AclSizeInformation))
            return String();

         const DWORD writes = AdminUpdateChecks::WriteRights;
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

      // The groups that hold ordinary accounts, or programs in an app container,
      // as SIDs: what the program's folders of mail, logs, keys and scripts, and
      // hMailServer.INI, are closed to (RunProtectCommand), and what its CHECK
      // lines name. A domain's Domain Users group (S-1-5-21-...-513) is one of
      // them too (IsBroad_).
      const wchar_t *const BroadSids_[] = {
         L"S-1-1-0",       // Everyone
         L"S-1-2-0",       // LOCAL
         L"S-1-2-1",       // CONSOLE LOGON
         L"S-1-5-2",       // NETWORK
         L"S-1-5-3",       // BATCH
         L"S-1-5-4",       // INTERACTIVE
         L"S-1-5-6",       // SERVICE
         L"S-1-5-7",       // ANONYMOUS LOGON
         L"S-1-5-11",      // Authenticated Users
         L"S-1-5-13",      // TERMINAL SERVER USER
         L"S-1-5-14",      // REMOTE INTERACTIVE LOGON
         L"S-1-5-32-545",  // Users
         L"S-1-5-32-546",  // Guests
         L"S-1-5-32-547",  // Power Users
         L"S-1-5-32-555",  // Remote Desktop Users
         L"S-1-15-2-1",    // ALL APPLICATION PACKAGES
         L"S-1-15-2-2",    // ALL RESTRICTED APPLICATION PACKAGES
      };

      bool IsBroad_(const String &sidText)
      {
         for (const wchar_t *broad : BroadSids_)
         {
            if (sidText.CompareNoCase(broad) == 0)
               return true;
         }
         return sidText.Left(9).CompareNoCase(_T("S-1-5-21-")) == 0 && sidText.Right(4) == _T("-513");
      }

      // An account as Windows names it - BUILTIN\Users, APPLICATION PACKAGE
      // AUTHORITY\ALL APPLICATION PACKAGES - or its SID when it has no name.
      String AccountName_(PSID sid)
      {
         wchar_t name[256];
         wchar_t domain[256];
         DWORD nameLength = 256;
         DWORD domainLength = 256;
         SID_NAME_USE use;
         if (!LookupAccountSidW(nullptr, sid, name, &nameLength, domain, &domainLength, &use))
            return SidText_(sid);
         return domain[0] != 0 ? String(domain) + _T("\\") + String(name) : String(name);
      }

      // The broad groups (IsBroad_) an allow entry that applies to what is at path
      // gives one of the rights, each as its name and its SID. Empty when none
      // has one, or when the permissions cannot be read.
      std::vector<std::pair<String, String> > BroadWithRights_(const String &path, DWORD rights)
      {
         std::vector<std::pair<String, String> > who;
         Permissions_ permissions;
         String error;
         if (!ReadPermissions_(path, permissions, error))
            return who;
         if (permissions.dacl == nullptr)
         {
            who.push_back(std::make_pair(String(_T("Everyone (it has no permissions list at all)")), String(_T("S-1-1-0"))));
            return who;
         }

         ACL_SIZE_INFORMATION size;
         if (!GetAclInformation(permissions.dacl, &size, sizeof(size), AclSizeInformation))
            return who;

         for (DWORD index = 0; index < size.AceCount; index++)
         {
            LPVOID entry = nullptr;
            if (!GetAce(permissions.dacl, index, &entry) || entry == nullptr)
               continue;

            const ACE_HEADER *header = (const ACE_HEADER *) entry;
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE) != 0)
               continue;

            const ACCESS_ALLOWED_ACE *allow = (const ACCESS_ALLOWED_ACE *) entry;
            if ((FileRights_(allow->Mask) & rights) == 0)
               continue;

            const String sidText = SidText_((PSID) &allow->SidStart);
            if (!IsBroad_(sidText))
               continue;

            bool listed = false;
            for (const auto &already : who)
               listed = listed || already.second.CompareNoCase(sidText) == 0;
            if (!listed)
               who.push_back(std::make_pair(AccountName_((PSID) &allow->SidStart), sidText));
         }

         return who;
      }

      // The broad groups that may read what is at path - list a folder, read a
      // file: what the program's secrets are closed to.
      std::vector<std::pair<String, String> > ReadableByBroad_(const String &path)
      {
         return BroadWithRights_(path, FILE_READ_DATA);
      }

      // The broad groups that may change what is at path: what the program's own
      // folders are closed to (ServiceAccount::Harden).
      std::vector<std::pair<String, String> > WritableByBroad_(const String &path)
      {
         return BroadWithRights_(path, AdminUpdateChecks::WriteRights);
      }

      // The names alone, "A, B and C".
      String NamesOf_(const std::vector<std::pair<String, String> > &who)
      {
         String names;
         for (size_t index = 0; index < who.size(); index++)
         {
            if (index > 0)
               names += index + 1 == who.size() ? _T(" and ") : _T(", ");
            names += who[index].first;
         }
         return names;
      }

      bool IsDenyEntry_(BYTE type)
      {
         return type == ACCESS_DENIED_ACE_TYPE || type == ACCESS_DENIED_OBJECT_ACE_TYPE ||
            type == ACCESS_DENIED_CALLBACK_ACE_TYPE || type == ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE;
      }

      // Whether permissions are already what PrivateAcl_ makes: inherited from
      // nothing - so nothing a folder above hands down reaches it - and, with
      // serviceReads, letting the service's SID read. A list that is not there at
      // all, which is everybody's full control, is not private however it is
      // marked.
      bool AlreadyPrivate_(const Permissions_ &permissions, PSID service, bool serviceReads)
      {
         if (!IsProtected_(permissions) || permissions.dacl == nullptr)
            return false;
         if (!serviceReads)
            return true;
         bool explicitDeny = false;
         bool inheritedDeny = false;
         return Covers_(permissions.dacl, service, ServiceAccount::RightsRead, false, explicitDeny, inheritedDeny);
      }

      // Whether a list of permissions lets every user of the computer read what
      // it is on - an allow entry that applies to it, not only to what is below,
      // giving Everyone, Authenticated Users or BUILTIN\Users the right to read
      // a file or list a folder - or is no list at all, which is everybody's
      // full control. What Program Files and a drive's root hand down, and what
      // a folder made for the purpose that only the administrators may open
      // does not: ServiceAccount::Harden gives BUILTIN\Users Read & execute
      // where the first was so, and not where it was not.
      bool EveryUserReads_(PACL dacl)
      {
         if (dacl == nullptr)
            return true;

         ACL_SIZE_INFORMATION size;
         if (!GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation))
            return false;

         for (DWORD index = 0; index < size.AceCount; index++)
         {
            LPVOID entry = nullptr;
            if (!GetAce(dacl, index, &entry) || entry == nullptr)
               continue;

            const ACE_HEADER *header = (const ACE_HEADER *) entry;
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE) != 0)
               continue;

            const ACCESS_ALLOWED_ACE *allow = (const ACCESS_ALLOWED_ACE *) entry;
            if ((FileRights_(allow->Mask) & FILE_READ_DATA) == 0)
               continue;

            const String sidText = SidText_((PSID) &allow->SidStart);
            if (sidText == _T("S-1-1-0") || sidText == _T("S-1-5-11") || sidText == _T("S-1-5-32-545"))
               return true;
         }
         return false;
      }

      // What a list of permissions is rebuilt for: a folder or a file of the
      // server's secrets, private to the administrators and the service
      // (PrivateAcl_), or a folder of its programs, which every user may read
      // and run and nobody but SYSTEM and the administrators may change
      // (HardenedAcl_).
      enum AclShape_
      {
         ShapePrivate_,
         ShapeHardened_
      };

      // The permissions a folder or a file is rebuilt with: no longer inherited,
      // so that what the folder above hands down - Users' Read from Program
      // Files, the right to make files and folders that a folder at a drive's
      // root hands every user on Windows Server, Authenticated Users' Modify on a
      // Windows client - stops here. Of what it had:
      //   - every deny entry is kept, an inherited one as an entry of its own -
      //     a deny only ever takes away;
      //   - every explicit entry is kept: an administrator's own, and what
      //     /GrantServiceAccess gave the service's SID - but on a program folder
      //     whose owner is not SYSTEM, the Administrators or TrustedInstaller
      //     (distrustOwn), an allow entry of its own that gives anybody else a
      //     right to write is dropped and named, as is one of another kind: that
      //     owner - a user who made the folder at a drive's root before the
      //     installation - could have written it, and would keep the folder
      //     through it once it is the Administrators';
      //   - an inherited allow entry for the service's SID is kept as an entry of
      //     its own - the Read on hMailServer.INI that Bin's grant hands down -
      //     and, on a program folder (ShapeHardened_), only while it gives no
      //     right to write (AdminUpdateChecks::WriteRights);
      //   - SYSTEM and the Administrators get full control, handed down to
      //     everything below, in place of what they inherited;
      //   - every other inherited allow entry is dropped, and named in dropped -
      //     BUILTIN\Users, ALL APPLICATION PACKAGES, CREATOR OWNER and the rest;
      //     on a program folder only one that gave a right to write is named,
      //     Authenticated Users' Modify from a drive's root above all, since
      //     what may read and run there is given back by the entry below;
      //   - on a program folder (ShapeHardened_), BUILTIN\Users gets Read &
      //     execute, handed down, when nothing of its own gives it that and the
      //     folder let every user read it before (EveryUserReads_) - a fresh
      //     entry, whatever the folder above handed them, so that the folder ends
      //     as Program Files leaves one; and none where it did not, since what
      //     the folder holds besides the six - a backup, a key file, a data
      //     folder of another name - would then be every user's to read, which
      //     it was not;
      //   - with serviceReads, the service's SID gets Read when nothing above
      //     gives it that: hMailServer.INI, which the service reads under every
      //     account and writes under none but LocalSystem.
      // Deny entries first, as Windows' canonical order has them. nullptr, with
      // detail, when it cannot be built.
      PACL RebuiltAcl_(PACL existing, PSID service, bool directory, AclShape_ shape, bool serviceReads, bool distrustOwn, std::vector<BYTE> &buffer, std::vector<String> &dropped, String &detail)
      {
         BYTE systemBuffer[SECURITY_MAX_SID_SIZE];
         BYTE administratorsBuffer[SECURITY_MAX_SID_SIZE];
         BYTE usersBuffer[SECURITY_MAX_SID_SIZE];
         DWORD systemSize = sizeof(systemBuffer);
         DWORD administratorsSize = sizeof(administratorsBuffer);
         DWORD usersSize = sizeof(usersBuffer);
         PSID system = (PSID) systemBuffer;
         PSID administrators = (PSID) administratorsBuffer;
         PSID users = (PSID) usersBuffer;
         if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &systemSize) ||
             !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &administratorsSize) ||
             !CreateWellKnownSid(WinBuiltinUsersSid, nullptr, users, &usersSize))
         {
            detail = Formatter::Format(_T("the SIDs of SYSTEM, the Administrators and the Users could not be made (Windows error {0})"), (int) GetLastError());
            return nullptr;
         }

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

         // At most three entries added: SYSTEM's and the Administrators', and the
         // service's Read (ShapePrivate_) or the Users' Read & execute
         // (ShapeHardened_).
         const DWORD extra = 3 * (DWORD) (sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE);
         const DWORD aclSize = (size.AclBytesInUse + extra + sizeof(DWORD) - 1) & ~((DWORD) sizeof(DWORD) - 1);
         buffer.assign(aclSize, 0);
         PACL acl = (PACL) buffer.data();
         if (!InitializeAcl(acl, aclSize, revision))
         {
            detail = Formatter::Format(_T("the new permissions could not be built (Windows error {0})"), (int) GetLastError());
            return nullptr;
         }

         // An entry copied as one of the folder's own: the same entry with its
         // inherited mark taken off.
         auto copyOwn = [&](const ACE_HEADER *header) -> bool
         {
            std::vector<BYTE> copy((const BYTE *) header, (const BYTE *) header + header->AceSize);
            ((ACE_HEADER *) copy.data())->AceFlags &= (BYTE) ~INHERITED_ACE;
            if (AddAce(acl, revision, MAXDWORD, copy.data(), header->AceSize))
               return true;
            detail = Formatter::Format(_T("an existing entry could not be copied (Windows error {0})"), (int) GetLastError());
            return false;
         };

         auto name = [&](const String &dropping)
         {
            if (std::find(dropped.begin(), dropped.end(), dropping) == dropped.end())
               dropped.push_back(dropping);
         };

         const BYTE handDown = directory ? (BYTE) (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) : (BYTE) 0;
         for (int pass = 0; pass < 3; pass++)
         {
            // Pass 0 the deny entries, pass 1 the explicit allow entries, pass 2
            // SYSTEM's and the Administrators' full control and the inherited
            // entries for the service's SID.
            if (pass == 2)
            {
               // Not again where an entry of the object's own already gives it - a
               // file made private before, whose service Read an administrator
               // took away, is given that back and nothing twice.
               for (PSID full : { system, administrators })
               {
                  bool explicitDeny = false;
                  bool inheritedDeny = false;
                  if (Covers_(acl, full, FILE_ALL_ACCESS, directory, explicitDeny, inheritedDeny))
                     continue;
                  if (!AddAccessAllowedAceEx(acl, revision, handDown, FILE_ALL_ACCESS, full))
                  {
                     detail = Formatter::Format(_T("SYSTEM's and the Administrators' entries could not be added (Windows error {0})"), (int) GetLastError());
                     return nullptr;
                  }
               }
            }

            for (DWORD index = 0; existing != nullptr && index < size.AceCount; index++)
            {
               LPVOID entry = nullptr;
               if (!GetAce(existing, index, &entry) || entry == nullptr)
               {
                  detail = Formatter::Format(_T("entry {0} of its permissions could not be read"), (int) index);
                  return nullptr;
               }

               const ACE_HEADER *header = (const ACE_HEADER *) entry;
               const bool inherited = (header->AceFlags & INHERITED_ACE) != 0;
               const bool deny = IsDenyEntry_(header->AceType);

               if (pass == 0)
               {
                  if (deny && !copyOwn(header))
                     return nullptr;
                  continue;
               }
               if (deny)
                  continue;

               if (pass == 1)
               {
                  if (inherited)
                     continue;
                  if (shape == ShapeHardened_ && distrustOwn)
                  {
                     if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
                     {
                        name(_T("an entry of its own of another kind"));
                        continue;
                     }
                     const ACCESS_ALLOWED_ACE *own = (const ACCESS_ALLOWED_ACE *) entry;
                     if ((FileRights_(own->Mask) & AdminUpdateChecks::WriteRights) != 0 && !IsTrustedWriter_((PSID) &own->SidStart))
                     {
                        name(AccountName_((PSID) &own->SidStart));
                        continue;
                     }
                  }
                  if (!copyOwn(header))
                     return nullptr;
                  continue;
               }

               if (!inherited)
                  continue;

               if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
               {
                  name(_T("an inherited entry of another kind"));
                  continue;
               }

               const ACCESS_ALLOWED_ACE *allow = (const ACCESS_ALLOWED_ACE *) entry;
               const PSID who = (PSID) &allow->SidStart;
               const bool writes = (FileRights_(allow->Mask) & AdminUpdateChecks::WriteRights) != 0;
               if (EqualSid(who, service) && (shape == ShapePrivate_ || !writes))
               {
                  if (!copyOwn(header))
                     return nullptr;
               }
               else if (!EqualSid(who, system) && !EqualSid(who, administrators))
               {
                  if (shape == ShapePrivate_ || writes)
                     name(AccountName_(who));
               }
            }
         }

         bool explicitDeny = false;
         bool inheritedDeny = false;
         if (shape == ShapeHardened_ && EveryUserReads_(existing) && !Covers_(acl, users, ServiceAccount::RightsReadExecute, directory, explicitDeny, inheritedDeny) && !explicitDeny)
         {
            if (!AddAccessAllowedAceEx(acl, revision, handDown, ServiceAccount::RightsReadExecute, users))
            {
               detail = Formatter::Format(_T("the Users' Read & execute could not be added (Windows error {0})"), (int) GetLastError());
               return nullptr;
            }
         }

         explicitDeny = false;
         inheritedDeny = false;
         if (serviceReads && !Covers_(acl, service, ServiceAccount::RightsRead, false, explicitDeny, inheritedDeny) && !explicitDeny)
         {
            if (!AddAccessAllowedAceEx(acl, revision, handDown, ServiceAccount::RightsRead, service))
            {
               detail = Formatter::Format(_T("the service's Read could not be added (Windows error {0})"), (int) GetLastError());
               return nullptr;
            }
         }

         return acl;
      }

      // The permissions a folder or a file of the server's secrets is given
      // (RebuiltAcl_, ShapePrivate_).
      PACL PrivateAcl_(PACL existing, PSID service, bool directory, bool serviceReads, std::vector<BYTE> &buffer, std::vector<String> &dropped, String &detail)
      {
         return RebuiltAcl_(existing, service, directory, ShapePrivate_, serviceReads, false, buffer, dropped, detail);
      }

      // The permissions a folder of the server's programs is given - the program
      // folder, Bin, ControlPanel (RebuiltAcl_, ShapeHardened_): SYSTEM and the
      // Administrators with full control, BUILTIN\Users Read & execute where
      // every user could read the folder before, each handed down, and nothing
      // the folder above hands down. distrustOwn: its owner was not SYSTEM, the
      // Administrators or TrustedInstaller, and its own entries that let anybody
      // else write go too.
      PACL HardenedAcl_(PACL existing, PSID service, bool directory, bool distrustOwn, std::vector<BYTE> &buffer, std::vector<String> &dropped, String &detail)
      {
         return RebuiltAcl_(existing, service, directory, ShapeHardened_, false, distrustOwn, buffer, dropped, detail);
      }

      // Whether a program folder's permissions and owner are already what
      // ServiceAccount::Harden makes: a list that is there and inherits nothing;
      // SYSTEM and the Administrators with full control, on a directory each
      // reaching both the folders and the files below it; and an owner that is
      // SYSTEM, the Administrators or TrustedInstaller, since an owner may
      // always change the permissions. BUILTIN\Users' entry is not asked for:
      // Harden gives it only where every user could read the folder before, and
      // an administrator who took it away from a folder already hardened chose
      // that. An entry of the folder's own that lets a broad group write is the
      // administrator's, kept, and said by the CHECK after the hardening.
      bool AlreadyHardened_(PACL dacl, bool isProtected, PSID owner, bool directory)
      {
         if (!isProtected || dacl == nullptr || owner == nullptr || !IsTrustedWriter_(owner))
            return false;

         const WELL_KNOWN_SID_TYPE kinds[] = { WinLocalSystemSid, WinBuiltinAdministratorsSid };
         for (WELL_KNOWN_SID_TYPE kind : kinds)
         {
            BYTE sidBuffer[SECURITY_MAX_SID_SIZE];
            DWORD sidSize = sizeof(sidBuffer);
            if (!CreateWellKnownSid(kind, nullptr, (PSID) sidBuffer, &sidSize))
               return false;

            bool explicitDeny = false;
            bool inheritedDeny = false;
            if (!Covers_(dacl, (PSID) sidBuffer, FILE_ALL_ACCESS, directory, explicitDeny, inheritedDeny))
               return false;
         }
         return true;
      }

      // The names dropped, "A, B and C".
      String DroppedNames_(const std::vector<String> &dropped)
      {
         String names;
         for (size_t index = 0; index < dropped.size(); index++)
         {
            if (index > 0)
               names += index + 1 == dropped.size() ? _T(" and ") : _T(", ");
            names += dropped[index];
         }
         return names;
      }

      // A file of secrets made private (PrivateAcl_) through a handle opened
      // without following links, with every folder above it held (PathPin_), and
      // only when it is a file with one name: a hard link elsewhere would carry
      // the permissions to wherever else the file is. GrantAlreadyThere, and
      // nothing written, when its permissions are private already.
      ServiceAccount::GrantResult ProtectFile_(const String &file, const String &sidText, bool serviceReads, String &detail, std::vector<String> &dropped, const Guarded_ *guarded)
      {
         detail.Empty();
         dropped.clear();

         PSID sid = nullptr;
         if (!ConvertStringSidToSidW(sidText.c_str(), &sid))
         {
            detail = Formatter::Format(_T("{0} is not a SID"), sidText);
            return ServiceAccount::GrantFailed;
         }
         SidRelease_ releaseSid(sid);

         PathPin_ pin;
         const String folder = FolderOf_(Normalised_(file));
         if (folder.IsEmpty() || !pin.Pin(folder, detail, guarded))
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
            detail = Formatter::Format(_T("it has {0} names on the disk (hard links), and permissions set on it would reach it under every one of them; it was left as it is"), (int) information.nNumberOfLinks);
            return ServiceAccount::GrantFailed;
         }

         Permissions_ permissions;
         const DWORD read = GetSecurityInfo(handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &permissions.dacl, nullptr, &permissions.descriptor);
         if (read != ERROR_SUCCESS)
         {
            detail = Formatter::Format(_T("its permissions could not be read (Windows error {0})"), (int) read);
            return ServiceAccount::GrantFailed;
         }
         if (AlreadyPrivate_(permissions, sid, serviceReads))
            return ServiceAccount::GrantAlreadyThere;

         std::vector<BYTE> buffer;
         PACL acl = PrivateAcl_(permissions.dacl, sid, false, serviceReads, buffer, dropped, detail);
         if (acl == nullptr)
            return ServiceAccount::GrantFailed;

         const DWORD written = SetSecurityInfo(handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
         if (written != ERROR_SUCCESS)
         {
            detail = Formatter::Format(_T("its permissions could not be written (Windows error {0})"), (int) written);
            return ServiceAccount::GrantFailed;
         }
         return ServiceAccount::GrantMade;
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
         if (RefusedLocation_(path, guarded, why) || !check.Pin(path, why, &guarded))
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
         if (check.Pin(Resolved_(folder.path), why, &guarded))
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
         if (!pin.Pin(target, detail, &guarded))
         {
            if (pin.Missing())
            {
               notes.push_back(Formatter::Format(_T("{0} does not exist, so it is not granted ({1})."), target, what));
               return;
            }
            sayResult(GrantFailed, rights, target, what, below, detail);
            return;
         }

         const GrantResult result = Grant(pin.GrantPath(target), sid, rights, below, detail);
         sayResult(result, rights, target, what, below, detail);
         if (result != GrantFailed && !pin.Through().IsEmpty())
            SayLine_(false, Formatter::Format(_T("                  through a link only the administrators may change, to {0}."), pin.Through()));
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
            const GrantResult result = GrantFile_(grant.path, sid, grant.rights == RightsModify ? RightsModify : RightsRead, detail, &guarded);
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
      //
      // A link among them - Events kept on another drive behind a junction - is
      // gone through when the administrators alone control it, as everywhere
      // here; any other is said and left, as every link here was until 6.3.6.
      for (const auto &entry : Subfolders_(program))
      {
         const String &subfolder = entry.first;
         bool writable = false;
         for (const wchar_t *name : WritableDefaults_)
            writable = writable || SamePath_(subfolder, FileUtilities::Combine(program, String(name)));
         if (SamePath_(subfolder, FileUtilities::Combine(program, String(_T("AdminUpdate")))))
            continue;
         if (writable)
            continue;

         if (entry.second)
         {
            PathPin_ check;
            String why;
            if (!check.Pin(subfolder, why, &guarded))
            {
               notes.push_back(Formatter::Format(_T("{0} in the program folder is not granted Read: {1}."), subfolder, why));
               continue;
            }
         }

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
         if (!pin.Pin(Normalised_(folder), why, &guarded))
         {
            failed++;
            SayLine_(true, Formatter::Format(_T("  FAILED          Modify  {0} - the files with protected permissions in it: {1}"), folder, why));
            continue;
         }

         for (const String &file : FilesIn_(Normalised_(folder)))
         {
            String detail;
            const GrantResult result = GrantFile_(file, sid, RightsModify, detail, &guarded);
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
         const GrantResult result = GrantFile_(file.path, sid, RightsRead, detail, &guarded);
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
            const GrantResult result = GrantFile_(keyStore, sid, RightsModify, detail, &guarded);
            sayResult(result, RightsModify, Normalised_(keyStore), keyStoreMade ? String(_T("the REST API's key store, made empty for it")) : String(_T("the REST API's key store, which the service writes")), false, detail);
         }
      }

      // What nobody should be able to write, and the service least: the
      // program folder, Bin, and hMailServer.INI. Said, not changed - the grant
      // removes nobody's entry - with what to run. /ProtectFolders, which setup
      // runs at every installation and upgrade before this, takes away an entry
      // any of them inherits (Harden, and hMailServer.INI made private); what
      // is left to say is an entry of its own, which an administrator gave.
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
         SayLine_(true, Formatter::Format(_T("  CHECK           {0} - {1}: {2} may write it, and the service is {2} or one of them, so it can change what runs elevated. hMailServer.exe /ProtectFolders, run from an elevated prompt in the Bin folder - setup runs it at every installation and upgrade - takes away an entry it inherits; one of its own stays until icacls \"{0}\" /remove:g *{2} takes it away."),
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

   // ---- the program's secrets, private to the administrators and the service

   ServiceAccount::GrantResult
   ServiceAccount::Protect(const String &path, const String &sid, bool directory, bool serviceReads, String &detail, std::vector<String> &dropped)
   {
      detail.Empty();
      dropped.clear();

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
      if (AlreadyPrivate_(permissions, binary, serviceReads))
         return GrantAlreadyThere;

      std::vector<BYTE> buffer;
      PACL acl = PrivateAcl_(permissions.dacl, binary, directory, serviceReads, buffer, dropped, detail);
      if (acl == nullptr)
         return GrantFailed;

      // Windows hands the new entries down to everything below a folder as part
      // of this call, and takes away what the folder handed down before - on a
      // large message store, minutes the first time, and nothing after that,
      // since a second run finds the permissions private already. A file or a
      // folder below with permissions of its own (ProtectedFile) is left as it is.
      std::wstring writable(path.c_str());
      const DWORD result = SetNamedSecurityInfoW(&writable[0], SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
      if (result != ERROR_SUCCESS)
      {
         detail = Formatter::Format(_T("its permissions could not be written (Windows error {0})"), (int) result);
         return GrantFailed;
      }
      return GrantMade;
   }

   // ---- the program's own folders, the administrators' alone to change

   ServiceAccount::GrantResult
   ServiceAccount::Harden(const String &path, const String &sid, bool directory, String &detail, std::vector<String> &dropped, String &formerOwner)
   {
      detail.Empty();
      dropped.clear();
      formerOwner.Empty();

      PSID binary = nullptr;
      if (!ConvertStringSidToSidW(sid.c_str(), &binary))
      {
         detail = Formatter::Format(_T("{0} is not a SID"), sid);
         return GrantFailed;
      }
      SidRelease_ release(binary);

      BYTE administratorsBuffer[SECURITY_MAX_SID_SIZE];
      DWORD administratorsSize = sizeof(administratorsBuffer);
      PSID administrators = (PSID) administratorsBuffer;
      if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &administratorsSize))
      {
         detail = Formatter::Format(_T("the Administrators' SID could not be made (Windows error {0})"), (int) GetLastError());
         return GrantFailed;
      }

      // The owner as well as the list: an owner may always read and change the
      // permissions, whatever they say, so a folder a user made at a drive's root
      // before an administrator installed into it would stay that user's to open
      // again.
      Permissions_ permissions;
      PSID owner = nullptr;
      const DWORD read = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
         &owner, nullptr, &permissions.dacl, nullptr, &permissions.descriptor);
      if (read != ERROR_SUCCESS)
      {
         detail = Formatter::Format(_T("its permissions could not be read (Windows error {0})"), (int) read);
         return GrantFailed;
      }
      if (AlreadyHardened_(permissions.dacl, IsProtected_(permissions), owner, directory))
         return GrantAlreadyThere;

      // An owner that is not trusted could have written the folder's own
      // entries as well, so those that let anybody else write go with it.
      const bool untrustedOwner = owner == nullptr || !IsTrustedWriter_(owner);

      std::vector<BYTE> buffer;
      PACL acl = HardenedAcl_(permissions.dacl, binary, directory, untrustedOwner, buffer, dropped, detail);
      if (acl == nullptr)
         return GrantFailed;

      SECURITY_INFORMATION which = DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;
      PSID newOwner = nullptr;
      if (untrustedOwner)
      {
         which |= OWNER_SECURITY_INFORMATION;
         newOwner = administrators;
         formerOwner = owner == nullptr ? String(_T("nobody")) : AccountName_(owner);
      }

      // Windows hands the new entries down to everything below the folder as
      // part of this call and takes away what the folder handed down before; a
      // file or folder below with permissions of its own - hMailServer.INI made
      // private, Data and the rest - is left as it is, and an entry of a file's
      // own, the service's Modify on the REST API's key store among them, stays.
      std::wstring writable(path.c_str());
      const DWORD result = SetNamedSecurityInfoW(&writable[0], SE_FILE_OBJECT, which, newOwner, nullptr, acl, nullptr);
      if (result != ERROR_SUCCESS)
      {
         formerOwner.Empty();
         detail = Formatter::Format(_T("its permissions could not be written (Windows error {0})"), (int) result);
         return GrantFailed;
      }
      return GrantMade;
   }

   int
   ServiceAccount::RunProtectCommand(const ProtectOptions &options)
   {
      // Elevated, as the grant is, and for the same reason it writes nothing
      // where the service can write: the log folder is the service's, and a
      // line written there by an administrator's process could go wherever a
      // link the service planted leads (R3).
      Logger::ForbidWritesForThisProcess();

      if (!ProcessIsAdministrator())
      {
         SayLine_(true, _T("hMailServer.exe /ProtectFolders changes the permissions of the program's data, log, temp, database and event folders, of hMailServer.INI, and of the program folder, Bin and ControlPanel, which takes an administrator: run it from an elevated Command Prompt. Nothing was changed."));
         return 3;
      }

      String program;
      String executable;
      if (!ProgramFolderOfThisBinary_(program, executable))
      {
         SayLine_(true, Formatter::Format(_T("{0} is not in the Bin folder of an installation, and the folders this protects are found from there, never from hMailServer.INI. Nothing was changed."), executable));
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
      SayLine_(false, Formatter::Format(_T("The program folder is {0}, where this program is."), program));
      SayLine_(false, _T("Each of these is made private: SYSTEM and the Administrators have full control, NT SERVICE\\hMailServer what /GrantServiceAccess gives it, and Read on hMailServer.INI; every deny entry and every entry of its own is kept; and nothing the folder above hands down reaches it - the Read that Program Files hands BUILTIN\\Users above all, which let every user of this computer read the mail."));

      IniFileSettings *ini = IniFileSettings::Instance();
      ini->LoadSettings();
      const Guarded_ guarded = MachineGuarded_(program);

      int made = 0;
      int there = 0;
      int hardened = 0;
      int hardenedThere = 0;
      int failed = 0;
      int checks = 0;
      std::vector<String> notes;

      // Set when one of the folders below that are made private is there and
      // could not be: the program folder's permissions are then not changed
      // (below), since Windows would hand them down through it.
      bool secretFolderFailed = false;

      // From trusted places alone: the program folder this binary is in, as the
      // grant takes its folders. A folder hMailServer.INI or the database names
      // elsewhere is only checked, below - it may be shared on purpose, and the
      // configuration is not where an elevated program takes a path to change.
      struct Secret_
      {
         String path;
         String what;
         bool directory;
         bool serviceReads;
      };
      const String bin = FileUtilities::Combine(program, _T("Bin"));
      std::vector<Secret_> secrets;
      secrets.push_back({ FileUtilities::Combine(program, _T("Data")), _T("the message store, and the keys, certificates and update files in it"), true, false });
      secrets.push_back({ FileUtilities::Combine(program, _T("Logs")), _T("the logs - addresses, subjects, whole SMTP, IMAP and POP3 conversations - and the crash dumps"), true, false });
      secrets.push_back({ FileUtilities::Combine(program, _T("Temp")), _T("the messages being received and scanned"), true, false });
      secrets.push_back({ FileUtilities::Combine(program, _T("Database")), _T("the built-in database: the accounts, their password hashes, the stored secrets"), true, false });
      secrets.push_back({ FileUtilities::Combine(program, _T("Events")), _T("the event scripts, which may hold passwords"), true, false });
      secrets.push_back({ FileUtilities::Combine(bin, _T("hMailServer.INI")), _T("the database's password, the administrator password's hash and its second factor"), false, true });

      // What a folder or file below may still let a broad group read, by an
      // entry of its own: said, never changed.
      auto checkKept = [&](const String &shown, const String &readPath)
      {
         const std::vector<std::pair<String, String> > who = ReadableByBroad_(readPath);
         if (who.empty())
            return;
         String sids;
         for (const auto &entry : who)
            sids += _T(" *") + entry.second;
         checks++;
         SayLine_(true, Formatter::Format(_T("  CHECK           {0} - {1} may still read it, by an entry of its own on it, which an administrator gave and which is kept. If they should not: icacls \"{0}\" /remove:g{2}"),
            shown, NamesOf_(who), sids));
      };

      for (const Secret_ &secret : secrets)
      {
         const String target = Normalised_(secret.path);
         if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES)
         {
            notes.push_back(Formatter::Format(_T("{0} does not exist, so it is not protected ({1})."), target, secret.what));
            continue;
         }

         if (options.reportOnly)
         {
            const std::vector<std::pair<String, String> > who = ReadableByBroad_(target);
            SayLine_(false, Formatter::Format(_T("  would protect   {0} - {1}{2}"), target, secret.what,
               who.empty() ? String() : Formatter::Format(_T("; {0} may read it now"), NamesOf_(who))));
            continue;
         }

         String detail;
         std::vector<String> dropped;
         String through;
         String readPath = target;
         GrantResult result = GrantFailed;
         if (secret.directory)
         {
            PathPin_ pin;
            if (!pin.Pin(target, detail, &guarded))
            {
               if (pin.Missing())
               {
                  notes.push_back(Formatter::Format(_T("{0} does not exist, so it is not protected ({1})."), target, secret.what));
                  continue;
               }
            }
            else
            {
               readPath = pin.GrantPath(target);
               result = Protect(readPath, sid, true, secret.serviceReads, detail, dropped);
               through = pin.Through();
            }
         }
         else
            result = ProtectFile_(target, sid, secret.serviceReads, detail, dropped, &guarded);

         if (result == GrantMade)
         {
            made++;
            SayLine_(false, Formatter::Format(_T("  made private    {0} - {1}"), target, secret.what));
            if (!dropped.empty())
               SayLine_(false, Formatter::Format(_T("                  no longer handed down to it: {0}."), DroppedNames_(dropped)));
         }
         else if (result == GrantAlreadyThere)
         {
            there++;
            SayLine_(false, Formatter::Format(_T("  already private {0} - {1}"), target, secret.what));
         }
         else
         {
            failed++;
            if (secret.directory)
               secretFolderFailed = true;
            SayLine_(true, Formatter::Format(_T("  FAILED          {0} - {1}: {2}"), target, secret.what, detail));
            continue;
         }

         if (!through.IsEmpty())
            SayLine_(false, Formatter::Format(_T("                  through a link only the administrators may change, to {0}."), through));

         checkKept(target, readPath);
      }

      // The program folder, Bin and ControlPanel: hardened (Harden), not made
      // private - BUILTIN\Users runs the Control Panel and DBSetup from them,
      // and the service, under any account, its own programs - so that nobody
      // but SYSTEM and the Administrators may change a program the service runs
      // or an administrator runs elevated, a library beside it, or the
      // uninstaller. Under Program Files that is what the folder above hands
      // down already. At a drive's root it is not: on a Windows client every
      // folder made there inherits Authenticated Users' Modify, and on Windows
      // Server Users' right to make files and folders in it, so that any user of
      // the computer could replace hMailServer.exe, or put a DLL in Bin that
      // Windows loads into it before its own (the application folder is searched
      // first), which the service and the administrator's next elevated program
      // would run. Until 6.3.7 nothing changed that; /GrantServiceAccess said
      // CHECK, under the virtual account alone.
      //
      // After the six, so that the message store is behind Data's own permissions
      // before the program folder's change is handed down: Windows hands the
      // program folder's new permissions down to every folder in it that
      // inherits, and through a Data that could not be made private, to every
      // message in it. So the program folder is left as it is while a folder of
      // the six could not be made private; Bin and ControlPanel do not hold the
      // message store, and are hardened all the same. What is in them with
      // permissions of its own is not changed, and the check after them says it.
      struct ProgramFolder_
      {
         String path;
         String what;
      };
      std::vector<ProgramFolder_> programFolders;
      programFolders.push_back({ program, _T("the program folder: the uninstaller, and what the folders in it inherit") });
      programFolders.push_back({ bin, _T("the server, and the programs and libraries an administrator runs elevated") });
      programFolders.push_back({ FileUtilities::Combine(program, _T("ControlPanel")), _T("the Control Panel, which an administrator runs elevated") });

      SayLine_(false, _T("Each of these is hardened: SYSTEM and the Administrators have full control and BUILTIN\\Users Read & execute, handed down - the Users' only where every user could read it before; every deny entry and every entry of its own is kept; its owner, when it is not SYSTEM, the Administrators or TrustedInstaller, is made the Administrators, and the entries of its own that let anybody else change it go too; and nothing the folder above hands down reaches it - the Modify that a drive's root hands Authenticated Users on a Windows client above all, which let every user of this computer replace the programs the service and the administrators run."));

      for (const ProgramFolder_ &folder : programFolders)
      {
         const String target = Normalised_(folder.path);
         if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES)
         {
            notes.push_back(Formatter::Format(_T("{0} does not exist, so it is not hardened ({1})."), target, folder.what));
            continue;
         }

         if (options.reportOnly)
         {
            const std::vector<std::pair<String, String> > who = WritableByBroad_(target);
            SayLine_(false, Formatter::Format(_T("  would harden    {0} - {1}{2}"), target, folder.what,
               who.empty() ? String() : Formatter::Format(_T("; {0} may change it now"), NamesOf_(who))));
            continue;
         }

         if (secretFolderFailed && SamePath_(target, program))
         {
            failed++;
            SayLine_(true, Formatter::Format(_T("  FAILED          {0} - {1}: not changed while a folder in it above could not be made private, since Windows would hand its permissions down through that folder and everything in it. Run this again once that folder can be made private."),
               target, folder.what));
            continue;
         }

         String detail;
         std::vector<String> dropped;
         String formerOwner;
         String through;
         GrantResult result = GrantFailed;
         PathPin_ pin;
         if (!pin.Pin(target, detail, &guarded))
         {
            if (pin.Missing())
            {
               notes.push_back(Formatter::Format(_T("{0} does not exist, so it is not hardened ({1})."), target, folder.what));
               continue;
            }
         }
         else
         {
            result = Harden(pin.GrantPath(target), sid, true, detail, dropped, formerOwner);
            through = pin.Through();
         }

         if (result == GrantMade)
         {
            hardened++;
            SayLine_(false, Formatter::Format(_T("  hardened        {0} - {1}"), target, folder.what));
            if (!dropped.empty())
               SayLine_(false, Formatter::Format(_T("                  no longer on it - handed down to it, or its own from the owner it had - with the right to change it: {0}."), DroppedNames_(dropped)));
            if (!formerOwner.IsEmpty())
               SayLine_(false, Formatter::Format(_T("                  its owner was {0}, who could have changed its permissions again; it is now the Administrators."), formerOwner));
         }
         else if (result == GrantAlreadyThere)
         {
            hardenedThere++;
            SayLine_(false, Formatter::Format(_T("  already hardened {0} - {1}"), target, folder.what));
         }
         else
         {
            failed++;
            SayLine_(true, Formatter::Format(_T("  FAILED          {0} - {1}: {2}"), target, folder.what, detail));
            continue;
         }

         if (!through.IsEmpty())
            SayLine_(false, Formatter::Format(_T("                  through a link only the administrators may change, to {0}."), through));
      }

      // The rule an administrator's install of an update holds the installation
      // to before it runs anything from it (AdminUpdateChecks::CheckInstallation):
      // the folders above the program folder, the program folder, Bin and
      // ControlPanel with every folder, program and library in them,
      // hMailServer.INI and AdminUpdate. What the hardening above makes passes
      // it, so the two agree; what it leaves - an entry of a file's or a
      // folder's own, which an administrator gave, an owner that is not SYSTEM,
      // the Administrators or TrustedInstaller, a folder above the program
      // folder - is said with the command that removes it, as that install
      // would name it, and never changed here: the first that fails, as the
      // install stops at it.
      if (!options.reportOnly)
      {
         AdminUpdateChecks::Finding finding;
         if (!AdminUpdateChecks::CheckInstallation(std::wstring(program.c_str()), std::vector<std::wstring>(), finding))
         {
            checks++;
            const String object = finding.object.c_str();
            if (!finding.reason.empty())
            {
               SayLine_(true, Formatter::Format(_T("  CHECK           {0} - {1}. An administrator's install of an update from the Control Panel refuses to start while it is so."),
                  object, String(finding.reason.c_str())));
            }
            else
            {
               String how;
               if (finding.owner)
                  how = _T("as its owner, who may change its permissions");
               else if (finding.inherited && !finding.inheritedFrom.empty())
                  how = Formatter::Format(_T("by an entry it inherits from {0}"), String(finding.inheritedFrom.c_str()));
               else if (finding.inherited)
                  how = _T("by an entry it inherits");
               else
                  how = _T("by an entry of its own, which an administrator gave and which is kept");
               const String who = Formatter::Format(_T("{0} ({1})"), String(finding.principal.c_str()), String(finding.sid.c_str()));
               SayLine_(true, Formatter::Format(_T("  CHECK           {0} - {1} may {2}, {3}: a program an administrator runs from the installation could be one that account put there, and an administrator's install of an update from the Control Panel refuses to start while it can. Nothing outside the program folder, Bin and ControlPanel's own permissions is changed here. To take it away: {4}"),
                  object, who, String(finding.right.c_str()), how, String(finding.fix.c_str())));
            }
         }
      }

      // Each file or folder checked below once, by its full path.
      std::vector<String> checked;

      // Key and certificate files in the data folder with permissions of their
      // own that a broad group may read - one moved in from elsewhere keeps the
      // permissions it had there, and the data folder's no longer reach it.
      // Said, with the icacls that gives it the folder's.
      if (!options.reportOnly)
      {
         const String data = FileUtilities::Combine(program, _T("Data"));
         std::vector<std::pair<String, int> > folders;
         folders.push_back(std::make_pair(FileUtilities::Combine(data, _T("Certificates")), 0));
         folders.push_back(std::make_pair(FileUtilities::Combine(data, _T("DKIM")), 0));
         folders.push_back(std::make_pair(FileUtilities::Combine(data, _T("ACME")), 0));
         for (size_t index = 0; index < folders.size(); index++)
         {
            const String folder = folders[index].first;
            for (const String &file : FilesIn_(folder))
            {
               checked.push_back(Resolved_(file));
               const std::vector<std::pair<String, String> > who = ReadableByBroad_(file);
               if (who.empty())
                  continue;
               checks++;
               SayLine_(true, Formatter::Format(_T("  CHECK           {0} - a key or certificate file {1} may read: its permissions are its own, as a file moved here from elsewhere keeps them. icacls \"{0}\" /reset gives it the data folder's."),
                  file, NamesOf_(who)));
            }
            if (folders[index].second < 2)
            {
               for (const auto &below : Subfolders_(folder))
               {
                  if (!below.second)
                     folders.push_back(std::make_pair(below.first, folders[index].second + 1));
               }
            }
         }
      }

      // What the configuration names outside the program's own folders - a data,
      // log or temp folder moved there in hMailServer.INI, a folder an
      // administrator approved, the backup, archive and PST import folders, the
      // key files - checked for a broad group's Read, and said, never changed.
      std::vector<Named_> named;
      named.push_back({ ini->GetDataDirectory(), _T("the data folder ([Directories] DataFolder)"), true, false });
      named.push_back({ ini->GetLogDirectory(), _T("the log folder ([Directories] LogFolder)"), true, false });
      named.push_back({ ini->GetTempDirectory(), _T("the temp folder ([Directories] TempFolder)"), true, false });
      named.push_back({ ini->GetDatabaseDirectory(), _T("the database folder ([Directories] DatabaseFolder)"), true, false });
      named.push_back({ ini->GetEventDirectory(), _T("the event folder ([Directories] EventFolder)"), false, false });
      std::vector<std::pair<String, DWORD> > approved;
      String listWhy;
      if (ReadApproved_(approved, listWhy))
      {
         for (const auto &entry : approved)
            named.push_back({ entry.first, _T("approved for the service by an administrator"), true, false });
      }
      String databaseWhy;
      if (ini->GetDatabaseSettingsExists() && !ReadConfiguredPaths_(named, databaseWhy))
         notes.push_back(Formatter::Format(_T("The database could not be read ({0}), so the backup, archive and PST import folders and the key files it names were not checked."), databaseWhy.IsEmpty() ? String(_T("no reason was given")) : databaseWhy));

      for (const Named_ &item : named)
      {
         String path = item.path;
         path.Trim();
         if (path.IsEmpty() || !IsDrivePath_(path))
            continue;
         path = Resolved_(path);
         if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;

         bool covered = false;
         for (const Secret_ &secret : secrets)
            covered = covered || (secret.directory && IsUnder_(path, secret.path) && !item.isFile);
         for (const String &already : checked)
            covered = covered || SamePath_(already, path);
         if (covered)
            continue;
         checked.push_back(path);

         const std::vector<std::pair<String, String> > who = ReadableByBroad_(path);
         if (who.empty())
            continue;
         String sids;
         for (const auto &entry : who)
            sids += _T(" *") + entry.second;
         checks++;
         SayLine_(true, Formatter::Format(_T("  CHECK           {0} - {1}: {2} may read it. It is outside the program's own folders, so it is not changed here. If they should not: icacls \"{0}\" /inheritance:d, then icacls \"{0}\" /remove:g{3}"),
            path, item.what, NamesOf_(who), sids));
      }

      for (const String &note : notes)
         SayLine_(false, note);

      SayLine_(failed > 0, Formatter::Format(_T("Done: {0} made private, {1} already private, {2} hardened, {3} already hardened, "), made, there, hardened, hardenedThere) +
         Formatter::Format(_T("{0} failed, {1} to check."), failed, checks));
      return failed > 0 ? 1 : 0;
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

      // A junction (a mount-point reparse point) at link, leading to target, as
      // mklink /J makes one. No privilege is needed to make one.
      bool MakeJunction_(const String &link, const String &target)
      {
         if (!CreateDirectoryW(link.c_str(), nullptr))
            return false;
         HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
         if (handle == INVALID_HANDLE_VALUE)
         {
            RemoveDirectoryW(link.c_str());
            return false;
         }

         const std::wstring substitute = L"\\??\\" + std::wstring(target.c_str());
         const std::wstring print(target.c_str());
         const size_t pathBytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
         std::vector<unsigned char> buffer(8 + 8 + pathBytes);
         unsigned char *data = buffer.data();
         *(DWORD *) (data + 0) = IO_REPARSE_TAG_MOUNT_POINT;
         *(WORD *) (data + 4) = (WORD) (8 + pathBytes);
         *(WORD *) (data + 6) = 0;
         *(WORD *) (data + 8) = 0;
         *(WORD *) (data + 10) = (WORD) (substitute.size() * sizeof(wchar_t));
         *(WORD *) (data + 12) = (WORD) ((substitute.size() + 1) * sizeof(wchar_t));
         *(WORD *) (data + 14) = (WORD) (print.size() * sizeof(wchar_t));
         memcpy(data + 16, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
         memcpy(data + 16 + (substitute.size() + 1) * sizeof(wchar_t), print.c_str(), (print.size() + 1) * sizeof(wchar_t));

         DWORD returned = 0;
         const BOOL made = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data, (DWORD) buffer.size(), nullptr, 0, &returned, nullptr);
         CloseHandle(handle);
         if (!made)
            RemoveDirectoryW(link.c_str());
         return made != FALSE;
      }

      // The owner and a protected DACL, from SDDL, set on what is at path itself -
      // a link, not where it leads - as icacls /setowner and /inheritance:r with
      // /L set them. False when this process may not: giving an object the
      // Administrators as its owner takes an administrator.
      bool SetOwnSecurity_(const String &path, const wchar_t *sddl)
      {
         PSECURITY_DESCRIPTOR descriptor = nullptr;
         if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr))
            return false;

         BOOL present = FALSE, defaulted = FALSE;
         PACL dacl = nullptr;
         PSID owner = nullptr;
         GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
         GetSecurityDescriptorOwner(descriptor, &owner, &defaulted);

         HANDLE handle = CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC | WRITE_OWNER, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
         bool set = false;
         if (handle != INVALID_HANDLE_VALUE)
         {
            set = SetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                  owner, nullptr, dacl, nullptr) == ERROR_SUCCESS;
            CloseHandle(handle);
         }

         LocalFree(descriptor);
         return set;
      }

      // A path through a junction the administrators alone control is gone
      // through, to where it leads, and held there; one the service's account
      // could change is refused, naming that account - where the
      // administrators' junction leads to it, too; one to a folder a grant may
      // never reach is refused for that; and a grant through one is written on
      // the folder it leads to, which keeps what it inherits from its own
      // folder. Made in a folder of the tester's own whose permissions only the
      // administrators may change. Skipped only where this process is no
      // administrator, and so may not give a junction the Administrators as
      // owner: in an administrator's process a failure to is the test's.
      void TestLinks_(const String &directory, const String &sid)
      {
         const String home = FileUtilities::Combine(directory, _T("links"));
         const String target = FileUtilities::Combine(directory, _T("linked"));
         const String adminLink = FileUtilities::Combine(home, _T("administrators"));
         const String serviceLink = FileUtilities::Combine(home, _T("service"));
         const String chainedLink = FileUtilities::Combine(home, _T("chained"));
         if (!FileUtilities::CreateDirectory(home) || !FileUtilities::CreateDirectory(target) || !FileUtilities::CreateDirectory(FileUtilities::Combine(target, _T("inside"))))
            FailGrant_(_T("The tester's folders for links could not be made."));

         const wchar_t *const administratorsAlone = L"O:BAD:P(A;;FA;;;BA)(A;;FA;;;SY)(A;;0x1200a9;;;BU)";
         if (!SetOwnSecurity_(home, administratorsAlone))
         {
            if (ServiceAccount::ProcessIsAdministrator())
               FailGrant_(Formatter::Format(_T("An administrator's process could not give the tester's folder for links to the administrators alone (Windows error {0})."), (int) GetLastError()));
            return;
         }

         const bool madeAdmin = MakeJunction_(adminLink, Resolved_(target));
         const bool madeService = MakeJunction_(serviceLink, Resolved_(target));
         // The administrators' junction to the service's: as though the folder
         // an administrator's link leads to had been swapped for a junction by
         // the service's account.
         const bool madeChained = MakeJunction_(chainedLink, Resolved_(serviceLink));
         try
         {
            if (!madeAdmin || !madeService || !madeChained)
               FailGrant_(_T("The tester's junctions could not be made."));

            if (!SetOwnSecurity_(adminLink, administratorsAlone) || !SetOwnSecurity_(chainedLink, administratorsAlone))
               FailGrant_(_T("The administrators could not be given the tester's junction alone."));
            const String serviceMay = String(L"O:BAD:P(A;;FA;;;BA)(A;;FA;;;SY)(A;;0x1301bf;;;") + sid + _T(")");
            if (!SetOwnSecurity_(serviceLink, serviceMay.c_str()))
               FailGrant_(_T("The tester's second junction could not be given its permissions."));

            {
               PathPin_ pin;
               String why;
               if (!pin.Pin(FileUtilities::Combine(adminLink, _T("inside")), why))
                  FailGrant_(_T("A path through a junction only the administrators control was not held: ") + why);
               if (!SamePath_(pin.Through(), FileUtilities::Combine(Resolved_(target), _T("inside"))))
                  FailGrant_(_T("A path through a junction was not found to lead where the junction points: ") + pin.Through());
               if (pin.GrantPath(adminLink).Left(11).CompareNoCase(_T("\\\\?\\Volume{")) != 0)
                  FailGrant_(_T("A grant through a junction is not written by the path of where it leads: ") + pin.GrantPath(adminLink));
               if (RemoveDirectoryW(FileUtilities::Combine(target, _T("inside")).c_str()))
                  FailGrant_(_T("The folder a junction leads to could be removed while it was held."));
               if (RemoveDirectoryW(adminLink.c_str()))
                  FailGrant_(_T("A junction a path is held through could be removed while it was held."));
            }

            {
               PathPin_ pin;
               String why;
               if (pin.Pin(FileUtilities::Combine(serviceLink, _T("inside")), why) || pin.Missing())
                  FailGrant_(_T("A path through a junction the service's account may change was held."));
               if (why.Find(sid) < 0)
                  FailGrant_(_T("The refusal of a junction the service's account may change does not name that account: ") + why);
            }

            {
               // Where it leads is held to the refusals a folder named directly is.
               Guarded_ guarded;
               guarded.nowhereNear.push_back(Resolved_(target));
               PathPin_ pin;
               String why;
               if (pin.Pin(adminLink, why, &guarded) || why.Find(_T("never granted")) < 0)
                  FailGrant_(_T("A junction leading to a folder no grant may reach was gone through: ") + why);
            }

            {
               PathPin_ pin;
               String why;
               if (pin.Pin(FileUtilities::Combine(adminLink, _T("missing")), why) || !pin.Missing())
                  FailGrant_(_T("A path that is not there beyond a junction was not found missing."));
            }

            {
               // A junction only the administrators control, leading to one the
               // service's account may change: an open through the first follows
               // the second unasked, and where the two end - the tester's folder,
               // which a grant may reach - says nothing of it. Refused, naming that
               // account. Until 6.3.6's re-check it was held, and a grant written
               // wherever the service's account had pointed its junction.
               PathPin_ pin;
               String why;
               if (pin.Pin(FileUtilities::Combine(chainedLink, _T("inside")), why) || pin.Missing())
                  FailGrant_(_T("A path through the administrators' junction to a junction the service's account may change was held, to ") + pin.Through());
               if (why.Find(sid) < 0)
                  FailGrant_(_T("The refusal of a junction the service's account may change, reached through the administrators', does not name that account: ") + why);
            }

            {
               // A grant through the administrators' junction: made on the folder
               // it leads to, by its volume's name, and handed down; what that
               // folder inherits from its own folder - the service's entry the
               // tester's directory hands down - kept, which a grant written by the
               // junction's path would have taken from the junction's folder
               // instead, where there is nothing to inherit; and the junction
               // itself still the administrators' alone.
               const String through = ServiceAccount::ServiceSid(_T("hMailServerGrantThroughLink"));
               PathPin_ pin;
               String why;
               String detail;
               if (!pin.Pin(adminLink, why))
                  FailGrant_(_T("The administrators' junction was not held for a grant: ") + why);
               if (ServiceAccount::Grant(pin.GrantPath(adminLink), through, ServiceAccount::RightsModify, true, detail) != ServiceAccount::GrantMade)
                  FailGrant_(_T("A grant through the administrators' junction was not made: ") + detail);
               pin.Release();

               std::vector<std::pair<DWORD, BYTE>> own = OwnEntries_(target, through);
               if (own.size() != 1 || own[0].first != ServiceAccount::RightsModify ||
                   (own[0].second & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) != (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE))
                  FailGrant_(Formatter::Format(_T("The folder a junction leads to has {0} explicit entries for the account granted through it, not one Modify handed down."), (int) own.size()));
               if (!ServiceAccount::HasGrant(FileUtilities::Combine(target, _T("inside")), through, ServiceAccount::RightsModify, true, detail))
                  FailGrant_(_T("A grant through a junction was not handed down to the folder in the one it leads to."));
               if (!ServiceAccount::HasGrant(target, sid, ServiceAccount::RightsModify, true, detail))
                  FailGrant_(_T("A grant through a junction took away what the folder it leads to inherits from its own folder."));

               PathPin_ again;
               if (!again.Pin(adminLink, why))
                  FailGrant_(_T("After a grant through it the junction is no longer the administrators' alone: ") + why);
            }
         }
         catch (...)
         {
            RemoveDirectoryW(chainedLink.c_str());
            RemoveDirectoryW(adminLink.c_str());
            RemoveDirectoryW(serviceLink.c_str());
            throw;
         }

         RemoveDirectoryW(chainedLink.c_str());
         RemoveDirectoryW(adminLink.c_str());
         RemoveDirectoryW(serviceLink.c_str());
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

      // A DACL from SDDL written on path, as icacls would - protected when the
      // SDDL says D:P. The owner of a folder it made may: no privilege is needed.
      bool SetDacl_(const String &path, const String &sddl)
      {
         PSECURITY_DESCRIPTOR descriptor = nullptr;
         if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            return false;
         BOOL present = FALSE;
         BOOL defaulted = FALSE;
         PACL dacl = nullptr;
         GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
         SECURITY_DESCRIPTOR_CONTROL control = 0;
         DWORD revision = 0;
         GetSecurityDescriptorControl(descriptor, &control, &revision);
         const SECURITY_INFORMATION which = DACL_SECURITY_INFORMATION |
            ((control & SE_DACL_PROTECTED) != 0 ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION);
         std::wstring writable(path.c_str());
         const bool set = SetNamedSecurityInfoW(&writable[0], SE_FILE_OBJECT, which, nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
         LocalFree(descriptor);
         return set;
      }

      // Whether path's DACL begins with an explicit deny entry for sid, as a
      // canonical list has its denies first, and holds no inherited entry.
      bool DeniesFirst_(const String &path, const String &sid)
      {
         Permissions_ permissions;
         String error;
         if (!ReadPermissions_(path, permissions, error) || permissions.dacl == nullptr)
            return false;
         LPVOID entry = nullptr;
         if (!GetAce(permissions.dacl, 0, &entry) || entry == nullptr)
            return false;
         const ACE_HEADER *header = (const ACE_HEADER *) entry;
         if (header->AceType != ACCESS_DENIED_ACE_TYPE || (header->AceFlags & INHERITED_ACE) != 0)
            return false;
         return SidText_((PSID) &((const ACCESS_DENIED_ACE *) entry)->SidStart).CompareNoCase(sid) == 0;
      }

      // What /ProtectFolders does to a folder and a file of secrets, on folders
      // of the tester's own that hand down what Program Files hands a default
      // installation's - BUILTIN\Users' Read - beside a deny entry and Read for
      // the service's SID (Bin's grant): Users no longer reach the folder, a file
      // already in it or one made after; the deny, an administrator's own entry
      // and the service's grant stay; SYSTEM and the Administrators have full
      // control handed down; the file keeps the service's Read as one of its
      // own, and one that had none is given it; a second run changes nothing;
      // and a file with two names is refused.
      void TestProtect_(const String &directory, const String &sid, const String &other)
      {
         // What is made private is SYSTEM's and the Administrators': a tester that
         // is neither could not write in the folder after, nor read it back.
         // Skipped there alone, as TestLinks_ is; the bench's service is LocalSystem.
         if (!ServiceAccount::ProcessIsAdministrator())
            return;

         const String backupOperators = _T("S-1-5-32-551");
         const String users = _T("S-1-5-32-545");
         const String outer = FileUtilities::Combine(directory, _T("protect"));
         const String bare = FileUtilities::Combine(directory, _T("protect-bare"));
         if (!FileUtilities::CreateDirectory(outer) || !FileUtilities::CreateDirectory(bare))
            FailGrant_(_T("The tester's folders for /ProtectFolders could not be made."));

         // As Program Files hands it down, with a deny entry for another SID and
         // Read for the service's (Bin's grant).
         const String handed = Formatter::Format(_T("D:P(D;OICI;FA;;;{0})(A;OICI;0x1200a9;;;BU)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;{1})"), other, sid);
         if (!SetDacl_(outer, handed) || !SetDacl_(bare, _T("D:P(A;OICI;0x1200a9;;;BU)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)")))
            FailGrant_(_T("The tester's folders could not be given the permissions Program Files hands down."));

         const String secret = FileUtilities::Combine(outer, _T("Data"));
         const String message = FileUtilities::Combine(secret, _T("message.eml"));
         const String ini = FileUtilities::Combine(outer, _T("hMailServer.INI"));
         const String bareIni = FileUtilities::Combine(bare, _T("hMailServer.INI"));
         if (!FileUtilities::CreateDirectory(secret) || !FileUtilities::WriteToFile(message, AnsiString("Subject: secret")) ||
             !FileUtilities::WriteToFile(ini, AnsiString("[Database]")) || !FileUtilities::WriteToFile(bareIni, AnsiString("[Database]")))
            FailGrant_(_T("The tester's folder and files for /ProtectFolders could not be made."));

         String detail;
         if (ServiceAccount::Grant(secret, backupOperators, ServiceAccount::RightsReadExecute, true, detail) != ServiceAccount::GrantMade ||
             ServiceAccount::Grant(secret, sid, ServiceAccount::RightsModify, true, detail) != ServiceAccount::GrantMade)
            FailGrant_(_T("The administrator's entry and the service's grant could not be made on the tester's folder: ") + detail);

         // Not vacuous: Users may read all three before.
         if (ReadableByBroad_(secret).empty() || ReadableByBroad_(message).empty() || ReadableByBroad_(ini).empty())
            FailGrant_(_T("The tester's folder and files do not let BUILTIN\\Users read them before /ProtectFolders, so nothing below would prove anything."));

         std::vector<String> dropped;
         if (ServiceAccount::Protect(secret, sid, true, false, detail, dropped) != ServiceAccount::GrantMade)
            FailGrant_(_T("The folder was not made private: ") + detail);
         if (!ProtectedPath_(secret))
            FailGrant_(_T("The folder made private still inherits from the folder above it."));
         if (dropped.empty() || DroppedNames_(dropped).Find(_T("Users")) < 0)
            FailGrant_(_T("Making the folder private does not name BUILTIN\\Users among the entries no longer handed down: ") + DroppedNames_(dropped));
         if (!ReadableByBroad_(secret).empty() || !ReadableByBroad_(message).empty())
            FailGrant_(_T("BUILTIN\\Users may still read the folder made private, or the message already in it: ") + NamesOf_(ReadableByBroad_(message)));
         const String after = FileUtilities::Combine(secret, _T("after.eml"));
         if (!FileUtilities::WriteToFile(after, AnsiString("Subject: after")) || !ReadableByBroad_(after).empty())
            FailGrant_(_T("A message made in the folder after it was made private may be read by BUILTIN\\Users."));
         if (!DeniesFirst_(secret, other))
            FailGrant_(_T("The inherited deny entry was not kept as the folder's own, first in its list."));
         if (OwnEntries_(secret, backupOperators).size() != 1)
            FailGrant_(_T("The administrator's own entry on the folder did not survive /ProtectFolders."));
         if (!ServiceAccount::HasGrant(secret, sid, ServiceAccount::RightsModify, true, detail) ||
             !ServiceAccount::HasGrant(message, sid, ServiceAccount::RightsModify, false, detail))
            FailGrant_(_T("The service's grant on the folder, or what the message inherits of it, did not survive /ProtectFolders."));
         for (const wchar_t *full : { L"S-1-5-18", L"S-1-5-32-544" })
         {
            if (!ServiceAccount::HasGrant(secret, String(full), FILE_ALL_ACCESS, true, detail) || !ServiceAccount::HasGrant(message, String(full), FILE_ALL_ACCESS, false, detail))
               FailGrant_(Formatter::Format(_T("{0} does not have full control of the folder made private and of what is in it."), String(full)));
         }
         if (ServiceAccount::Protect(secret, sid, true, false, detail, dropped) != ServiceAccount::GrantAlreadyThere)
            FailGrant_(_T("A second /ProtectFolders on the same folder did not find it private already."));

         // hMailServer.INI: Users dropped, the service's Read - inherited from
         // Bin's grant - kept as the file's own, and given where nothing gave it.
         if (ProtectFile_(ini, sid, true, detail, dropped, nullptr) != ServiceAccount::GrantMade)
            FailGrant_(_T("hMailServer.INI was not made private: ") + detail);
         if (!ProtectedPath_(ini) || !ReadableByBroad_(ini).empty())
            FailGrant_(_T("BUILTIN\\Users may still read the hMailServer.INI made private."));
         if (!ServiceAccount::HasGrant(ini, sid, ServiceAccount::RightsRead, false, detail))
            FailGrant_(_T("The service may no longer read the hMailServer.INI made private."));
         if (ProtectFile_(ini, sid, true, detail, dropped, nullptr) != ServiceAccount::GrantAlreadyThere)
            FailGrant_(_T("A second /ProtectFolders on hMailServer.INI did not find it private already."));

         // A file made private whose service Read an administrator took away is
         // given it back, and SYSTEM's and the Administrators' entries are not
         // written twice.
         if (!SetDacl_(ini, _T("D:P(A;;FA;;;SY)(A;;FA;;;BA)")) || ServiceAccount::HasGrant(ini, sid, ServiceAccount::RightsRead, false, detail))
            FailGrant_(_T("The tester could not take the service's Read off its hMailServer.INI."));
         if (ProtectFile_(ini, sid, true, detail, dropped, nullptr) != ServiceAccount::GrantMade || !ServiceAccount::HasGrant(ini, sid, ServiceAccount::RightsRead, false, detail))
            FailGrant_(_T("A private hMailServer.INI without the service's Read was not given it back."));
         if (OwnEntries_(ini, _T("S-1-5-18")).size() != 1 || OwnEntries_(ini, _T("S-1-5-32-544")).size() != 1)
            FailGrant_(Formatter::Format(_T("hMailServer.INI made private again has {0} entries for SYSTEM and {1} for the Administrators; one each was expected."),
               (int) OwnEntries_(ini, _T("S-1-5-18")).size(), (int) OwnEntries_(ini, _T("S-1-5-32-544")).size()));
         if (ProtectFile_(bareIni, sid, true, detail, dropped, nullptr) != ServiceAccount::GrantMade ||
             !ServiceAccount::HasGrant(bareIni, sid, ServiceAccount::RightsRead, false, detail) || !ReadableByBroad_(bareIni).empty())
            FailGrant_(_T("An hMailServer.INI the service could not read before was not given its Read when made private."));

         // A file with a second name is refused, and keeps what it had.
         const String linked = FileUtilities::Combine(outer, _T("linked.ini"));
         const String second = FileUtilities::Combine(outer, _T("second.ini"));
         if (FileUtilities::WriteToFile(linked, AnsiString("[Database]")) && CreateHardLinkW(second.c_str(), linked.c_str(), nullptr))
         {
            if (ProtectFile_(second, sid, true, detail, dropped, nullptr) != ServiceAccount::GrantFailed || detail.IsEmpty())
               FailGrant_(_T("A file with two names was made private through one of them."));
            if (ProtectedPath_(linked) || ReadableByBroad_(linked).empty())
               FailGrant_(_T("The refused file with two names had its permissions changed all the same."));
         }

         // What Users is not: the folder's own Read for Users, which an
         // administrator gave, is kept - and reported (ReadableByBroad_).
         if (ServiceAccount::Grant(secret, users, ServiceAccount::RightsReadExecute, true, detail) != ServiceAccount::GrantMade || ReadableByBroad_(secret).empty())
            FailGrant_(_T("An entry of its own for BUILTIN\\Users, given after /ProtectFolders, is not seen as letting Users read the folder."));
         if (ServiceAccount::Protect(secret, sid, true, false, detail, dropped) != ServiceAccount::GrantAlreadyThere || ReadableByBroad_(secret).empty())
            FailGrant_(_T("/ProtectFolders took away an entry of the folder's own that an administrator gave."));
      }

      // The allow entries for sid in a list, inherited or not, as mask and flags.
      std::vector<std::pair<DWORD, BYTE>> AllowsFor_(PACL acl, PSID sid)
      {
         std::vector<std::pair<DWORD, BYTE>> entries;
         ACL_SIZE_INFORMATION size;
         if (acl == nullptr || !GetAclInformation(acl, &size, sizeof(size), AclSizeInformation))
            return entries;
         for (DWORD index = 0; index < size.AceCount; index++)
         {
            LPVOID entry = nullptr;
            if (!GetAce(acl, index, &entry) || entry == nullptr)
               continue;
            const ACE_HEADER *header = (const ACE_HEADER *) entry;
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
               continue;
            const ACCESS_ALLOWED_ACE *allow = (const ACCESS_ALLOWED_ACE *) entry;
            if (EqualSid((PSID) &allow->SidStart, sid))
               entries.push_back(std::make_pair(allow->Mask, header->AceFlags));
         }
         return entries;
      }

      // The first allow entry in a list - one that applies or one only handed
      // down - that gives somebody but SYSTEM, the Administrators or
      // TrustedInstaller a right to write: what a hardened folder, and what is
      // made in it, may not hold. Empty when there is none.
      String UntrustedWriter_(PACL acl)
      {
         ACL_SIZE_INFORMATION size;
         if (acl == nullptr)
            return _T("Everyone (no list at all)");
         if (!GetAclInformation(acl, &size, sizeof(size), AclSizeInformation))
            return _T("(the list could not be read)");
         for (DWORD index = 0; index < size.AceCount; index++)
         {
            LPVOID entry = nullptr;
            if (!GetAce(acl, index, &entry) || entry == nullptr)
               continue;
            const ACE_HEADER *header = (const ACE_HEADER *) entry;
            if (IsDenyEntry_(header->AceType))
               continue;
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
               return _T("(an allow entry of another kind)");
            const ACCESS_ALLOWED_ACE *allow = (const ACCESS_ALLOWED_ACE *) entry;
            if ((FileRights_(allow->Mask) & AdminUpdateChecks::WriteRights) != 0 && !IsTrustedWriter_((PSID) &allow->SidStart))
               return SidText_((PSID) &allow->SidStart);
         }
         return String();
      }

      // What Harden builds for a program folder (HardenedAcl_) from what each
      // kind of folder above hands one down, held to the rule an administrator's
      // install of an update holds the program folder, Bin and ControlPanel to
      // (AdminUpdateChecks::EvaluateDescriptor, PlaceInside), in memory - so
      // that it runs for a tester that is no administrator too. A folder at a
      // Windows client's drive root, whose Authenticated Users' Modify fails the
      // rule before, and Windows Server's, whose Users may make files and
      // folders in it and CREATOR OWNER have full control of them; a folder in
      // Program Files, which passes before; one an inherited Modify for the
      // service reaches, and one its inherited Read does; one with a deny entry
      // and an administrator's entry of its own; one with no list at all; and
      // one in a folder only the administrators may open. After: each passes
      // the rule, nobody but SYSTEM, the Administrators and TrustedInstaller
      // may write it or what is made in it, SYSTEM and the Administrators have
      // full control handed down, Users Read & execute handed down where every
      // user could read the folder before and no entry at all where none could,
      // the entries that gave a right to write are named and Users' Read is not,
      // the grant's Read, the deny (first) and the administrator's entry are
      // kept; and AlreadyHardened_ takes the list for hardened when it is
      // protected and the Administrators own it, and not when it is not
      // protected or a user owns it.
      void TestHardenedShape_(const String &sid, const String &other)
      {
         struct Shape_
         {
            String what;
            String sddl;
            bool passesBefore;
            String droppedSid;
            String notDroppedSid;
            String keptSid;
            bool usersRead;
         };

         const String trustedInstaller = AdminUpdateChecks::TrustedInstallerSid;
         std::vector<Shape_> shapes;
         shapes.push_back({ _T("Bin at a Windows client's drive root, with the grant's Read"),
            Formatter::Format(_T("D:AI(A;OICI;0x1200a9;;;{0})(A;OICIID;FA;;;BA)(A;OICIID;FA;;;SY)(A;OICIID;0x1200a9;;;BU)(A;OICIID;0x1301bf;;;AU)"), sid),
            false, _T("S-1-5-11"), _T("S-1-5-32-545"), sid, true });
         shapes.push_back({ _T("a folder at a Windows Server's drive root"),
            _T("D:AI(A;OICIID;FA;;;BA)(A;OICIID;FA;;;SY)(A;OICIID;0x1200a9;;;BU)(A;CIID;0x4;;;BU)(A;CIID;0x2;;;BU)(A;OICIIOID;GA;;;CO)"),
            false, _T("S-1-3-0"), String(), String(), true });
         shapes.push_back({ _T("a folder in Program Files"),
            Formatter::Format(_T("D:AI(A;ID;FA;;;{0})(A;CIIOID;GA;;;{0})(A;ID;0x1301bf;;;SY)(A;OICIIOID;GA;;;SY)(A;ID;0x1301bf;;;BA)(A;OICIIOID;GA;;;BA)(A;ID;0x1200a9;;;BU)(A;OICIIOID;GXGR;;;BU)(A;OICIIOID;GA;;;CO)(A;ID;0x1200a9;;;AC)(A;OICIIOID;GXGR;;;AC)"), trustedInstaller),
            true, _T("S-1-3-0"), _T("S-1-5-32-545"), String(), true });
         shapes.push_back({ _T("a folder an inherited Modify for the service reaches"),
            Formatter::Format(_T("D:AI(A;OICIID;0x1301bf;;;{0})(A;OICIID;FA;;;BA)(A;OICIID;FA;;;SY)"), sid),
            false, sid, String(), String(), false });
         shapes.push_back({ _T("a folder the service's inherited Read reaches"),
            Formatter::Format(_T("D:AI(A;OICIID;0x1200a9;;;{0})(A;OICIID;FA;;;BA)(A;OICIID;FA;;;SY)(A;OICIID;0x1200a9;;;BU)"), sid),
            true, String(), sid, sid, true });
         shapes.push_back({ _T("a folder with a deny entry and an administrator's entry of its own"),
            Formatter::Format(_T("D:AI(D;OICI;FA;;;{0})(A;OICI;0x1200a9;;;BO)(A;OICIID;FA;;;BA)(A;OICIID;FA;;;SY)(A;OICIID;0x1301bf;;;AU)"), other),
            false, _T("S-1-5-11"), String(), _T("S-1-5-32-551"), true });
         shapes.push_back({ _T("a folder with no list at all"), _T("D:NO_ACCESS_CONTROL"), false, String(), String(), String(), true });
         shapes.push_back({ _T("a folder in one only the administrators may open"), _T("D:AI(A;OICIID;FA;;;BA)(A;OICIID;FA;;;SY)"),
            true, String(), String(), String(), false });

         PSID service = nullptr;
         PSID userLike = nullptr;
         if (!ConvertStringSidToSidW(sid.c_str(), &service) || !ConvertStringSidToSidW(_T("S-1-5-21-1004336348-1177238915-682003330-1001"), &userLike))
            FailGrant_(_T("The tester's SIDs could not be made."));
         SidRelease_ releaseService(service);
         SidRelease_ releaseUser(userLike);

         BYTE administratorsBuffer[SECURITY_MAX_SID_SIZE];
         BYTE systemBuffer[SECURITY_MAX_SID_SIZE];
         BYTE usersBuffer[SECURITY_MAX_SID_SIZE];
         DWORD administratorsSize = sizeof(administratorsBuffer);
         DWORD systemSize = sizeof(systemBuffer);
         DWORD usersSize = sizeof(usersBuffer);
         PSID administrators = (PSID) administratorsBuffer;
         PSID system = (PSID) systemBuffer;
         PSID users = (PSID) usersBuffer;
         if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &administratorsSize) ||
             !CreateWellKnownSid(WinLocalSystemSid, nullptr, system, &systemSize) ||
             !CreateWellKnownSid(WinBuiltinUsersSid, nullptr, users, &usersSize))
            FailGrant_(_T("The SIDs of SYSTEM, the Administrators and the Users could not be made."));

         for (const Shape_ &shape : shapes)
         {
            PSECURITY_DESCRIPTOR before = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW((_T("O:BA") + shape.sddl).c_str(), SDDL_REVISION_1, &before, nullptr))
               FailGrant_(Formatter::Format(_T("The SDDL of {0} does not parse."), shape.what));

            AdminUpdateChecks::Finding finding;
            const bool passedBefore = AdminUpdateChecks::EvaluateDescriptor(before, AdminUpdateChecks::PlaceInside, finding);

            BOOL present = FALSE;
            BOOL defaulted = FALSE;
            PACL dacl = nullptr;
            GetSecurityDescriptorDacl(before, &present, &dacl, &defaulted);

            std::vector<BYTE> buffer;
            std::vector<String> dropped;
            String detail;
            PACL hardened = HardenedAcl_(present ? dacl : nullptr, service, true, false, buffer, dropped, detail);
            const String writerBefore = UntrustedWriter_(present ? dacl : nullptr);
            LocalFree(before);

            if (!shape.passesBefore && writerBefore.IsEmpty())
               FailGrant_(Formatter::Format(_T("The case {0} gives nobody a right to write before, so it proves nothing."), shape.what));

            if (passedBefore != shape.passesBefore)
               FailGrant_(Formatter::Format(_T("{0} {1} the rule of an administrator's install before it is hardened, which the case says it does not."), shape.what,
                  passedBefore ? String(_T("passes")) : String(_T("fails"))));
            if (hardened == nullptr)
               FailGrant_(Formatter::Format(_T("The hardened permissions of {0} could not be built: {1}"), shape.what, detail));

            SECURITY_DESCRIPTOR after;
            if (!InitializeSecurityDescriptor(&after, SECURITY_DESCRIPTOR_REVISION) || !SetSecurityDescriptorOwner(&after, administrators, FALSE) ||
                !SetSecurityDescriptorDacl(&after, TRUE, hardened, FALSE) || !SetSecurityDescriptorControl(&after, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
               FailGrant_(_T("The tester could not make a descriptor of the hardened permissions."));

            if (!AdminUpdateChecks::EvaluateDescriptor(&after, AdminUpdateChecks::PlaceInside, finding))
               FailGrant_(Formatter::Format(_T("{0}, hardened, fails the rule of an administrator's install: {1} ({2}) may {3}."), shape.what,
                  String(finding.principal.c_str()), String(finding.sid.c_str()), String(finding.right.c_str())));

            const String writer = UntrustedWriter_(hardened);
            if (!writer.IsEmpty())
               FailGrant_(Formatter::Format(_T("{0}, hardened, still lets {1} write it or what is made in it."), shape.what, writer));

            bool explicitDeny = false;
            bool inheritedDeny = false;
            if (!Covers_(hardened, system, FILE_ALL_ACCESS, true, explicitDeny, inheritedDeny) || !Covers_(hardened, administrators, FILE_ALL_ACCESS, true, explicitDeny, inheritedDeny))
               FailGrant_(Formatter::Format(_T("{0}, hardened, does not give SYSTEM and the Administrators full control handed down."), shape.what));
            if (shape.usersRead && !Covers_(hardened, users, ServiceAccount::RightsReadExecute, true, explicitDeny, inheritedDeny))
               FailGrant_(Formatter::Format(_T("{0}, hardened, does not let BUILTIN\\Users read and run what is in it."), shape.what));
            if (!shape.usersRead && !AllowsFor_(hardened, users).empty())
               FailGrant_(Formatter::Format(_T("{0}, hardened, lets BUILTIN\\Users read what is in it, which no user of the computer could before."), shape.what));

            if (!AlreadyHardened_(hardened, true, administrators, true))
               FailGrant_(Formatter::Format(_T("{0}, hardened, protected and owned by the Administrators, is not taken for hardened."), shape.what));
            if (AlreadyHardened_(hardened, false, administrators, true))
               FailGrant_(Formatter::Format(_T("{0}, hardened but still inheriting, is taken for hardened."), shape.what));
            if (AlreadyHardened_(hardened, true, userLike, true))
               FailGrant_(Formatter::Format(_T("{0}, hardened but owned by a user, who may change its permissions, is taken for hardened."), shape.what));

            auto namedAmongDropped = [&](const String &sidText) -> bool
            {
               PSID binary = nullptr;
               if (!ConvertStringSidToSidW(sidText.c_str(), &binary))
                  return false;
               const String name = AccountName_(binary);
               LocalFree(binary);
               return std::find(dropped.begin(), dropped.end(), name) != dropped.end();
            };
            if (!shape.droppedSid.IsEmpty() && !namedAmongDropped(shape.droppedSid))
               FailGrant_(Formatter::Format(_T("Hardening {0} does not name {1} among the entries no longer handed down: {2}"), shape.what, shape.droppedSid, DroppedNames_(dropped)));
            if (!shape.notDroppedSid.IsEmpty() && namedAmongDropped(shape.notDroppedSid))
               FailGrant_(Formatter::Format(_T("Hardening {0} names {1} among the entries no longer handed down, which keeps what it may do: {2}"), shape.what, shape.notDroppedSid, DroppedNames_(dropped)));

            if (!shape.keptSid.IsEmpty())
            {
               PSID kept = nullptr;
               if (!ConvertStringSidToSidW(shape.keptSid.c_str(), &kept))
                  FailGrant_(_T("The tester's SID could not be made."));
               const std::vector<std::pair<DWORD, BYTE>> entries = AllowsFor_(hardened, kept);
               LocalFree(kept);
               if (entries.size() != 1 || entries[0].first != ServiceAccount::RightsReadExecute || (entries[0].second & INHERITED_ACE) != 0 ||
                   (entries[0].second & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) != (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE))
                  FailGrant_(Formatter::Format(_T("Hardening {0} did not keep {1}'s Read & execute, handed down, as one entry of its own."), shape.what, shape.keptSid));
            }

            if (shape.sddl.Find(_T("(D;")) >= 0)
            {
               LPVOID entry = nullptr;
               if (!GetAce(hardened, 0, &entry) || entry == nullptr || ((const ACE_HEADER *) entry)->AceType != ACCESS_DENIED_ACE_TYPE ||
                   SidText_((PSID) &((const ACCESS_DENIED_ACE *) entry)->SidStart).CompareNoCase(other) != 0)
                  FailGrant_(Formatter::Format(_T("Hardening {0} did not keep the deny entry first."), shape.what));
            }

         }

         // A folder a user made at a drive's root before the installation, and
         // gave its own Full control and Modify for Everyone: while the
         // Administrators own it the entries are theirs and kept; from an owner
         // Harden does not trust (distrustOwn) the two go and are named, and the
         // grant's Read and Users' Read & execute, which give no right to write,
         // stay.
         {
            PSECURITY_DESCRIPTOR made = nullptr;
            const String sddl = Formatter::Format(_T("O:BAD:AI(A;OICI;FA;;;S-1-5-21-1004336348-1177238915-682003330-1001)(A;OICI;0x1301bf;;;WD)(A;OICI;0x1200a9;;;{0})(A;OICIID;FA;;;BA)(A;OICIID;FA;;;SY)(A;OICIID;0x1200a9;;;BU)"), sid);
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &made, nullptr))
               FailGrant_(_T("The SDDL of a folder a user made does not parse."));
            BOOL present = FALSE;
            BOOL defaulted = FALSE;
            PACL dacl = nullptr;
            GetSecurityDescriptorDacl(made, &present, &dacl, &defaulted);

            PSID everyone = nullptr;
            ConvertStringSidToSidW(_T("S-1-1-0"), &everyone);
            SidRelease_ releaseEveryone(everyone);

            for (bool distrust : { false, true })
            {
               std::vector<BYTE> buffer;
               std::vector<String> dropped;
               String detail;
               PACL hardened = HardenedAcl_(present ? dacl : nullptr, service, true, distrust, buffer, dropped, detail);
               if (hardened == nullptr)
               {
                  LocalFree(made);
                  FailGrant_(_T("The hardened permissions of a folder a user made could not be built: ") + detail);
               }

               const size_t userEntries = AllowsFor_(hardened, userLike).size();
               const size_t everyoneEntries = AllowsFor_(hardened, everyone).size();
               if (!distrust && (userEntries != 1 || everyoneEntries != 1))
               {
                  LocalFree(made);
                  FailGrant_(_T("Hardening a folder the Administrators own took away an entry of its own."));
               }
               if (distrust)
               {
                  const bool named = std::find(dropped.begin(), dropped.end(), AccountName_(userLike)) != dropped.end() &&
                     std::find(dropped.begin(), dropped.end(), AccountName_(everyone)) != dropped.end();
                  if (userEntries != 0 || everyoneEntries != 0 || !named || !UntrustedWriter_(hardened).IsEmpty())
                  {
                     LocalFree(made);
                     FailGrant_(_T("Hardening a folder a user owned kept, or did not name, an entry of its own that let somebody else write it: ") + DroppedNames_(dropped));
                  }
                  bool explicitDeny = false;
                  bool inheritedDeny = false;
                  if (AllowsFor_(hardened, service).size() != 1 || !Covers_(hardened, users, ServiceAccount::RightsReadExecute, true, explicitDeny, inheritedDeny))
                  {
                     LocalFree(made);
                     FailGrant_(_T("Hardening a folder a user owned took away the grant's Read or the Users' Read & execute."));
                  }
               }
            }
            LocalFree(made);
         }
      }

      // What /ProtectFolders makes of an installation's own folders at a
      // Windows client's drive root, on folders of the tester's own: the
      // tester's folder hands down what C:\ does there - Authenticated Users'
      // Modify to everything made below it - and an installation is made in it,
      // the program folder with the uninstaller, Bin with the server, a library,
      // hMailServer.INI and the REST API's key store, and ControlPanel with the
      // Control Panel, with the grant's entries on them. Before, Authenticated
      // Users may change each, and the rule of an administrator's install refuses
      // them. Hardened: nobody but SYSTEM and the Administrators may change the
      // folders, the files in them or a file made after; Users may still read
      // and run them; the grant's Read on Bin and ControlPanel and the service's
      // Modify on the key store, entries of their own, stay; Authenticated Users
      // is named; the rule passes each, as AdminUpdateChecks::CheckObject holds
      // {app}, Bin and ControlPanel to it (CheckInstallation, without the folders
      // above the tester's, which are not the tester's); a second run changes
      // nothing; an entry an administrator gives Users afterwards is kept,
      // already hardened, and the rule names it, as the CHECK after the
      // hardening does; and a folder a user owns is given to the Administrators,
      // the Modify that user gave itself taken away, where the tester runs as an
      // administrator's own account and so may make
      // it the owner (under LocalSystem, the bench's service, it may not, and
      // that case is skipped). Skipped where this process is no administrator,
      // as TestProtect_ is: SYSTEM's and the Administrators' folders could not
      // be written, read back or removed by it.
      void TestHarden_(const String &directory, const String &sid)
      {
         if (!ServiceAccount::ProcessIsAdministrator())
            return;

         const String root = FileUtilities::Combine(directory, _T("harden-root"));
         if (!FileUtilities::CreateDirectory(root))
            FailGrant_(_T("The tester's folder for the hardening could not be made."));

         // A Windows client's C:\, as the probe of 6 October 2026 read it.
         if (!SetDacl_(root, _T("D:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;0x1200a9;;;BU)(A;OICIIO;0x1301bf;;;AU)(A;;0x4;;;AU)")))
            FailGrant_(_T("The tester's folder could not be given the permissions a Windows client's drive root hands down."));

         const String app = FileUtilities::Combine(root, _T("hMailServer"));
         const String bin = FileUtilities::Combine(app, _T("Bin"));
         const String panel = FileUtilities::Combine(app, _T("ControlPanel"));
         const String uninstaller = FileUtilities::Combine(app, _T("unins000.exe"));
         const String server = FileUtilities::Combine(bin, _T("hMailServer.exe"));
         const String library = FileUtilities::Combine(bin, _T("libcrypto-4-x64.dll"));
         const String ini = FileUtilities::Combine(bin, _T("hMailServer.INI"));
         const String keys = FileUtilities::Combine(bin, _T("hMailServerApiKeys.ini"));
         const String controlPanel = FileUtilities::Combine(panel, _T("hMailCP.exe"));
         if (!FileUtilities::CreateDirectory(app) || !FileUtilities::CreateDirectory(bin) || !FileUtilities::CreateDirectory(panel) ||
             !FileUtilities::WriteToFile(uninstaller, AnsiString("MZ")) || !FileUtilities::WriteToFile(server, AnsiString("MZ")) ||
             !FileUtilities::WriteToFile(library, AnsiString("MZ")) || !FileUtilities::WriteToFile(ini, AnsiString("[Database]")) ||
             !FileUtilities::WriteToFile(keys, AnsiString("")) || !FileUtilities::WriteToFile(controlPanel, AnsiString("MZ")))
            FailGrant_(_T("The tester's installation could not be made."));

         // The grant's entries, as /GrantServiceAccess leaves them: Read on Bin
         // and ControlPanel handed down, Modify on the key store alone; and
         // hMailServer.INI made private first, as /ProtectFolders does.
         String detail;
         std::vector<String> dropped;
         if (ServiceAccount::Grant(bin, sid, ServiceAccount::RightsReadExecute, true, detail) != ServiceAccount::GrantMade ||
             ServiceAccount::Grant(panel, sid, ServiceAccount::RightsReadExecute, true, detail) != ServiceAccount::GrantMade ||
             GrantFile_(keys, sid, ServiceAccount::RightsModify, detail) != ServiceAccount::GrantMade ||
             ProtectFile_(ini, sid, true, detail, dropped, nullptr) != ServiceAccount::GrantMade)
            FailGrant_(_T("The grant's entries could not be made on the tester's installation: ") + detail);

         const String everyFile[] = { uninstaller, server, library, keys, controlPanel };
         const String everyFolder[] = { app, bin, panel };

         // Not vacuous: Authenticated Users may change each, and the rule of an
         // administrator's install refuses them.
         AdminUpdateChecks::Finding finding;
         for (const String &path : everyFile)
         {
            if (WritableByBroad_(path).empty())
               FailGrant_(Formatter::Format(_T("{0} does not let Authenticated Users change it before it is hardened, so nothing below would prove anything."), path));
         }
         if (AdminUpdateChecks::CheckObject(std::wstring(server.c_str()), AdminUpdateChecks::PlaceInside, std::wstring(app.c_str()), finding) ||
             _wcsicmp(finding.sid.c_str(), L"S-1-5-11") != 0)
            FailGrant_(_T("The rule of an administrator's install does not refuse a server Authenticated Users may change."));

         // The program folder, Bin and ControlPanel, as /ProtectFolders hardens them.
         String formerOwner;
         for (const String &folder : everyFolder)
         {
            if (ServiceAccount::Harden(folder, sid, true, detail, dropped, formerOwner) != ServiceAccount::GrantMade)
               FailGrant_(Formatter::Format(_T("{0} was not hardened: {1}"), folder, detail));
            if (!formerOwner.IsEmpty())
               FailGrant_(Formatter::Format(_T("{0}, made by an administrator's process, was given another owner: it had {1}."), folder, formerOwner));
            if (!ProtectedPath_(folder))
               FailGrant_(Formatter::Format(_T("{0}, hardened, still inherits from the folder above it."), folder));
            PSID authenticatedUsers = nullptr;
            ConvertStringSidToSidW(_T("S-1-5-11"), &authenticatedUsers);
            const String name = AccountName_(authenticatedUsers);
            LocalFree(authenticatedUsers);
            if (SamePath_(folder, app) && std::find(dropped.begin(), dropped.end(), name) == dropped.end())
               FailGrant_(_T("Hardening the program folder does not name Authenticated Users among the entries no longer handed down: ") + DroppedNames_(dropped));
         }

         const String after = FileUtilities::Combine(bin, _T("after.dll"));
         if (!FileUtilities::WriteToFile(after, AnsiString("MZ")))
            FailGrant_(_T("A library could not be made in the hardened Bin."));

         std::vector<String> everything(std::begin(everyFile), std::end(everyFile));
         everything.insert(everything.end(), std::begin(everyFolder), std::end(everyFolder));
         everything.push_back(after);
         for (const String &path : everything)
         {
            const bool folder = FileUtilities::DirectoryExists(path);
            if (!WritableByBroad_(path).empty())
               FailGrant_(Formatter::Format(_T("{0} may still be changed by {1} after the hardening."), path, NamesOf_(WritableByBroad_(path))));
            if (!ServiceAccount::HasGrant(path, _T("S-1-5-32-545"), ServiceAccount::RightsReadExecute, folder, detail))
               FailGrant_(Formatter::Format(_T("BUILTIN\\Users may no longer read and run {0} after the hardening."), path));
            for (const wchar_t *full : { L"S-1-5-18", L"S-1-5-32-544" })
            {
               if (!ServiceAccount::HasGrant(path, String(full), FILE_ALL_ACCESS, folder, detail))
                  FailGrant_(Formatter::Format(_T("{0} does not have full control of {1} after the hardening."), String(full), path));
            }
            if (path == keys)
               continue;
            // What an administrator's install checks in the program folder, Bin
            // and ControlPanel: every folder, program and library.
            if (!AdminUpdateChecks::CheckObject(std::wstring(path.c_str()), AdminUpdateChecks::PlaceInside, std::wstring(app.c_str()), finding))
               FailGrant_(Formatter::Format(_T("{0}, hardened, fails the rule of an administrator's install: {1} ({2}) may {3}{4}."), path, String(finding.principal.c_str()),
                  String(finding.sid.c_str()), String(finding.right.c_str()), String(finding.reason.c_str())));
         }

         // The grant's entries, each the folder's or the file's own, kept.
         if (OwnEntries_(bin, sid).size() != 1 || OwnEntries_(panel, sid).size() != 1 ||
             !ServiceAccount::HasGrant(server, sid, ServiceAccount::RightsReadExecute, false, detail))
            FailGrant_(_T("The grant's Read on Bin and ControlPanel, or what the server inherits of it, did not survive the hardening."));
         if (!ServiceAccount::HasGrant(keys, sid, ServiceAccount::RightsModify, false, detail) || OwnEntries_(keys, sid).size() != 1)
            FailGrant_(_T("The service's Modify on the REST API's key store did not survive the hardening of Bin."));
         if (!ProtectedPath_(ini) || !ReadableByBroad_(ini).empty() || !ServiceAccount::HasGrant(ini, sid, ServiceAccount::RightsRead, false, detail))
            FailGrant_(_T("hMailServer.INI, private, was changed by the hardening of Bin."));
         if (!AdminUpdateChecks::CheckObject(std::wstring(ini.c_str()), AdminUpdateChecks::PlaceInside, std::wstring(app.c_str()), finding))
            FailGrant_(_T("hMailServer.INI, private in a hardened Bin, fails the rule of an administrator's install."));

         // A second run changes nothing.
         for (const String &folder : everyFolder)
         {
            if (ServiceAccount::Harden(folder, sid, true, detail, dropped, formerOwner) != ServiceAccount::GrantAlreadyThere || !formerOwner.IsEmpty())
               FailGrant_(Formatter::Format(_T("A second hardening of {0} did not find it hardened already."), folder));
         }

         // An entry an administrator gives Users afterwards is theirs: kept, the
         // folder still hardened, and the rule names it, as the CHECK after the
         // hardening does.
         if (ServiceAccount::Grant(panel, _T("S-1-5-32-545"), ServiceAccount::RightsModify, true, detail) != ServiceAccount::GrantMade)
            FailGrant_(_T("The tester could not give Users an entry of their own on ControlPanel: ") + detail);
         if (ServiceAccount::Harden(panel, sid, true, detail, dropped, formerOwner) != ServiceAccount::GrantAlreadyThere || OwnEntries_(panel, _T("S-1-5-32-545")).size() != 2)
            FailGrant_(_T("Hardening took away an entry of ControlPanel's own that an administrator gave Users, or did not find the folder hardened."));
         if (AdminUpdateChecks::CheckObject(std::wstring(controlPanel.c_str()), AdminUpdateChecks::PlaceInside, std::wstring(app.c_str()), finding) ||
             _wcsicmp(finding.sid.c_str(), L"S-1-5-32-545") != 0)
            FailGrant_(_T("The rule of an administrator's install does not name the Users' Modify an administrator gave ControlPanel."));

         // A folder a user owns - one made at a drive's root before the
         // installation, by anybody - is given to the Administrators. Only where
         // this process's own account may be made an owner and is not one the
         // rule trusts.
         std::vector<BYTE> userBuffer;
         PSID user = ProcessUserSid_(userBuffer);
         if (user != nullptr && !IsTrustedWriter_(user))
         {
            // And the Modify such an owner gives itself, an entry of the
            // folder's own, which goes with the owner.
            const String userText = SidText_(user);
            if (ServiceAccount::Grant(bin, userText, ServiceAccount::RightsModify, true, detail) != ServiceAccount::GrantMade)
               FailGrant_(_T("The tester could not give its own account Modify on Bin: ") + detail);
            std::wstring binPath(bin.c_str());
            if (SetNamedSecurityInfoW(&binPath[0], SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, user, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
               FailGrant_(_T("The tester could not make its own account the owner of Bin."));
            if (ServiceAccount::Harden(bin, sid, true, detail, dropped, formerOwner) != ServiceAccount::GrantMade || formerOwner.IsEmpty())
               FailGrant_(_T("Hardening a Bin a user owns did not give it to the Administrators: ") + detail);
            if (!OwnEntries_(bin, userText).empty() || OwnEntries_(bin, sid).size() != 1)
               FailGrant_(_T("Hardening a Bin a user owned kept the Modify that user gave itself, or took away the grant's Read."));

            PSECURITY_DESCRIPTOR descriptor = nullptr;
            PSID owner = nullptr;
            const bool ownedByAdministrators = GetNamedSecurityInfoW(bin.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &descriptor) == ERROR_SUCCESS &&
               owner != nullptr && IsWellKnownSid(owner, WinBuiltinAdministratorsSid);
            if (descriptor != nullptr)
               LocalFree(descriptor);
            if (!ownedByAdministrators)
               FailGrant_(_T("Bin, hardened, is not owned by the Administrators."));
            if (ServiceAccount::Harden(bin, sid, true, detail, dropped, formerOwner) != ServiceAccount::GrantAlreadyThere)
               FailGrant_(_T("Bin, given to the Administrators, is not found hardened."));
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

      // The permissions /ProtectFolders gives the program folder, Bin and
      // ControlPanel, built from what each kind of folder above hands down and
      // held to the rule of an administrator's install, in memory
      // (TestHardenedShape_).
      TestHardenedShape_(ServiceAccount::ServiceSid(_T("hMailServerGrantTest")), ServiceAccount::ServiceSid(_T("hMailServerNeverGranted")));

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

         // A junction the administrators alone control is gone through, and one
         // the service's account could change is refused (TestLinks_).
         TestLinks_(directory, sid);

         // What /ProtectFolders makes of a folder and a file of secrets that
         // inherit BUILTIN\Users' Read (TestProtect_).
         TestProtect_(directory, sid, other);

         // What /ProtectFolders makes of an installation's program folder, Bin
         // and ControlPanel that inherit Authenticated Users' Modify from a
         // Windows client's drive root (TestHarden_).
         TestHarden_(directory, sid);

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
