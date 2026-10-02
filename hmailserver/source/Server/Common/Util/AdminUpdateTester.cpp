// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#ifndef HM_PLATFORM_POSIX

#include "AdminUpdateTester.h"
#include "AdminUpdateChecks.h"
#include "AuthenticodePublisher.h"
#include "LoweredChild.h"

#include <aclapi.h>
#include <sddl.h>
#include <winioctl.h>
#include <wintrust.h>

#include <stdexcept>

// The sentence is built only when the condition fails, and after it has been
// evaluated: a function's arguments are evaluated in no set order, so a message
// that reads a variable the condition fills in must not be one of them.
#define Expect_(condition, what) do { if (!(condition)) HM::Fail_(what); } while (false)

namespace HM
{
   namespace
   {
      std::string Narrow_(const std::wstring &text)
      {
         if (text.empty())
            return std::string();
         const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int) text.size(), nullptr, 0, nullptr, nullptr);
         std::string out((size_t) needed, '\0');
         WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int) text.size(), &out[0], needed, nullptr, nullptr);
         return out;
      }

      [[noreturn]] void Fail_(const std::wstring &what)
      {
         throw std::logic_error("AdminUpdateTester: " + Narrow_(what));
      }

      std::wstring Join_(const std::wstring &folder, const std::wstring &name)
      {
         return folder.empty() || folder.back() == L'\\' ? folder + name : folder + L"\\" + name;
      }

      bool IsElevated_()
      {
         BYTE buffer[SECURITY_MAX_SID_SIZE];
         DWORD size = sizeof(buffer);
         BOOL member = FALSE;
         return CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, buffer, &size) && CheckTokenMembership(nullptr, buffer, &member) && member;
      }

      // A junction (a mount-point reparse point) at link, leading to target. No
      // privilege is needed to make one, which is why the stage must refuse them.
      bool MakeJunction_(const std::wstring &link, const std::wstring &target)
      {
         if (!CreateDirectoryW(link.c_str(), nullptr))
            return false;
         HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
         if (handle == INVALID_HANDLE_VALUE)
            return false;

         const std::wstring substitute = L"\\??\\" + target;
         const std::wstring print = target;
         const size_t pathBytes = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
         std::vector<unsigned char> buffer(8 + 8 + pathBytes);
         unsigned char *data = buffer.data();
         *(DWORD *) (data + 0) = IO_REPARSE_TAG_MOUNT_POINT;
         *(WORD *) (data + 4) = (WORD) (8 + pathBytes);
         *(WORD *) (data + 6) = 0;
         *(WORD *) (data + 8) = 0;                                                        // substitute offset
         *(WORD *) (data + 10) = (WORD) (substitute.size() * sizeof(wchar_t));            // substitute length
         *(WORD *) (data + 12) = (WORD) ((substitute.size() + 1) * sizeof(wchar_t));      // print offset
         *(WORD *) (data + 14) = (WORD) (print.size() * sizeof(wchar_t));                 // print length
         memcpy(data + 16, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
         memcpy(data + 16 + (substitute.size() + 1) * sizeof(wchar_t), print.c_str(), (print.size() + 1) * sizeof(wchar_t));

         DWORD returned = 0;
         const BOOL made = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data, (DWORD) buffer.size(), nullptr, 0, &returned, nullptr);
         CloseHandle(handle);
         return made != FALSE;
      }

      // What the tester made with permissions only administrators pass, given
      // back to whoever runs it - its owner, who may always change the DACL - so
      // that it can be removed.
      void Unlock_(const std::wstring &path)
      {
         const std::wstring me = AdminUpdateChecks::CurrentUserSid();
         PSECURITY_DESCRIPTOR descriptor = nullptr;
         ULONG size = 0;
         if (me.empty() || !ConvertStringSecurityDescriptorToSecurityDescriptorW((L"D:P(A;OICI;FA;;;" + me + L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)").c_str(),
                                                                                 SDDL_REVISION_1, &descriptor, &size))
            return;
         BOOL present = FALSE, defaulted = FALSE;
         PACL dacl = nullptr;
         if (GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) && present)
            SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, dacl, nullptr);
         LocalFree(descriptor);
      }

      void RemoveTree_(const std::wstring &path)
      {
         const DWORD attributes = GetFileAttributesW(path.c_str());
         if (attributes == INVALID_FILE_ATTRIBUTES)
            return;
         if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
            Unlock_(path);
         if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
         {
            WIN32_FIND_DATAW data;
            HANDLE search = FindFirstFileW(Join_(path, L"*").c_str(), &data);
            if (search != INVALID_HANDLE_VALUE)
            {
               do
               {
                  const std::wstring name = data.cFileName;
                  if (name != L"." && name != L"..")
                     RemoveTree_(Join_(path, name));
               } while (FindNextFileW(search, &data));
               FindClose(search);
            }
         }
         SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
         if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            RemoveDirectoryW(path.c_str());
         else
            DeleteFileW(path.c_str());
      }

      // This process's token with every SID in it made deny-only - its user too -
      // but Authenticated Users and Everyone, and no privilege but change-notify:
      // what an unelevated administrator's Control Panel can do with a folder whose
      // entries are SYSTEM's, the Administrators' and its owner's, and no more (a
      // deny-only SID is not the owner). NULL when it cannot be made.
      HANDLE AuthenticatedUsersOnlyToken_()
      {
         HANDLE own = nullptr;
         if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_IMPERSONATE, &own))
            return nullptr;
         DWORD groupsNeeded = 0, userNeeded = 0;
         GetTokenInformation(own, TokenGroups, nullptr, 0, &groupsNeeded);
         GetTokenInformation(own, TokenUser, nullptr, 0, &userNeeded);
         std::vector<unsigned char> groupsBuffer(groupsNeeded ? groupsNeeded : 1);
         std::vector<unsigned char> userBuffer(userNeeded ? userNeeded : 1);
         HANDLE restricted = nullptr;
         if (GetTokenInformation(own, TokenGroups, groupsBuffer.data(), (DWORD) groupsBuffer.size(), &groupsNeeded) &&
             GetTokenInformation(own, TokenUser, userBuffer.data(), (DWORD) userBuffer.size(), &userNeeded))
         {
            std::vector<SID_AND_ATTRIBUTES> disable;
            SID_AND_ATTRIBUTES user = ((const TOKEN_USER *) userBuffer.data())->User;
            user.Attributes = 0;
            disable.push_back(user);
            const TOKEN_GROUPS *groups = (const TOKEN_GROUPS *) groupsBuffer.data();
            for (DWORD i = 0; i < groups->GroupCount; i++)
            {
               SID_AND_ATTRIBUTES group = groups->Groups[i];
               if ((group.Attributes & SE_GROUP_INTEGRITY) != 0)
                  continue;
               const std::wstring text = AdminUpdateChecks::TextOfSid(group.Sid);
               if (text == L"S-1-5-11" || text == L"S-1-1-0")
                  continue;
               group.Attributes = 0;
               disable.push_back(group);
            }
            if (!CreateRestrictedToken(own, DISABLE_MAX_PRIVILEGE, (DWORD) disable.size(), disable.data(), 0, nullptr, 0, nullptr, &restricted))
               restricted = nullptr;
         }
         CloseHandle(own);
         return restricted;
      }

      // Whether that token may read a folder's owner and DACL, which is what the
      // Control Panel's permission check reads (READ_CONTROL). error is
      // Windows' answer when it may not.
      bool CanReadPermissionsAs_(HANDLE token, const std::wstring &path, DWORD &error)
      {
         error = 0;
         if (!ImpersonateLoggedOnUser(token))
         {
            error = GetLastError();
            return false;
         }
         DWORD needed = 0;
         const BOOL read = GetFileSecurityW(path.c_str(), OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, nullptr, 0, &needed);
         error = read ? ERROR_SUCCESS : GetLastError();
         RevertToSelf();
         return !read && error == ERROR_INSUFFICIENT_BUFFER;
      }

      bool WriteFile_(const std::wstring &path, const std::string &bytes)
      {
         HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
         if (file == INVALID_HANDLE_VALUE)
            return false;
         DWORD written = 0;
         const BOOL ok = bytes.empty() || WriteFile(file, bytes.data(), (DWORD) bytes.size(), &written, nullptr);
         CloseHandle(file);
         return ok && written == bytes.size();
      }

      std::string ReadAll_(const std::wstring &path)
      {
         HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
         if (file == INVALID_HANDLE_VALUE)
            return std::string();
         std::string out;
         char buffer[65536];
         DWORD got = 0;
         while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got > 0)
            out.append(buffer, got);
         CloseHandle(file);
         return out;
      }

      bool CopyWithChange_(const std::wstring &from, const std::wstring &to, long long flipAt, long long truncateBy, const std::string &append)
      {
         if (!CopyFileW(from.c_str(), to.c_str(), FALSE))
            return false;
         HANDLE file = CreateFileW(to.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
         if (file == INVALID_HANDLE_VALUE)
            return false;
         bool ok = true;
         LARGE_INTEGER size;
         GetFileSizeEx(file, &size);
         if (flipAt >= 0)
         {
            LARGE_INTEGER at;
            at.QuadPart = flipAt;
            unsigned char byte = 0;
            DWORD moved = 0;
            ok = SetFilePointerEx(file, at, nullptr, FILE_BEGIN) && ReadFile(file, &byte, 1, &moved, nullptr) && moved == 1;
            byte ^= 0x01;
            ok = ok && SetFilePointerEx(file, at, nullptr, FILE_BEGIN) && WriteFile(file, &byte, 1, &moved, nullptr) && moved == 1;
         }
         if (ok && truncateBy > 0)
         {
            LARGE_INTEGER end;
            end.QuadPart = size.QuadPart - truncateBy;
            ok = SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
         }
         if (ok && !append.empty())
         {
            LARGE_INTEGER zero;
            zero.QuadPart = 0;
            DWORD written = 0;
            ok = SetFilePointerEx(file, zero, nullptr, FILE_END) && WriteFile(file, append.data(), (DWORD) append.size(), &written, nullptr);
         }
         CloseHandle(file);
         return ok;
      }

      struct PermissionCase_
      {
         const wchar_t *name;
         AdminUpdateChecks::Place place;
         bool pass;
         const wchar_t *sid;
         bool owner;
         bool inherited;
         const wchar_t *sddl;
      };

      // build/testdata/install-permissions-cases.json, case for case and in the
      // same order; ControlPanel.Tests' InstallationPermissionsTests holds this
      // table to that file, so the two implementations cannot drift apart.
      // PERMISSION-CASES-BEGIN
      const PermissionCase_ PermissionCases_[] =
      {
         { L"program-files-passes-inside", AdminUpdateChecks::PlaceInside, true, L"", false, false, L"O:S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464D:PAI(A;;FA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)(A;CIIO;GA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)(A;;0x1301bf;;;SY)(A;OICIIO;GA;;;SY)(A;;0x1301bf;;;BA)(A;OICIIO;GA;;;BA)(A;;0x1200a9;;;BU)(A;OICIIO;GXGR;;;BU)(A;OICIIO;GA;;;CO)(A;;0x1200a9;;;AC)(A;OICIIO;GXGR;;;AC)" },
         { L"program-files-passes-above", AdminUpdateChecks::PlaceAboveApp, true, L"", false, false, L"O:S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464D:PAI(A;;FA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)(A;CIIO;GA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)(A;;0x1301bf;;;SY)(A;OICIIO;GA;;;SY)(A;;0x1301bf;;;BA)(A;OICIIO;GA;;;BA)(A;;0x1200a9;;;BU)(A;OICIIO;GXGR;;;BU)(A;OICIIO;GA;;;CO)(A;;0x1200a9;;;AC)(A;OICIIO;GXGR;;;AC)" },
         { L"installed-bin-with-the-service-reading-passes", AdminUpdateChecks::PlaceInside, true, L"", false, false, L"O:BAD:AI(A;OICI;0x1200a9;;;S-1-5-80-3504853920-2627644491-1723263464-2546768561-1700046809)(A;ID;0x1301bf;;;SY)(A;OICIIOID;GA;;;SY)(A;ID;0x1301bf;;;BA)(A;OICIIOID;GA;;;BA)(A;ID;0x1200a9;;;BU)(A;OICIIOID;GXGR;;;BU)(A;OICIIOID;GA;;;CO)(A;ID;0x1200a9;;;AC)" },
         { L"volume-root-passes-above", AdminUpdateChecks::PlaceAboveApp, true, L"", false, false, L"O:SYD:(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)(A;OICI;0x1200a9;;;BU)(A;OICIIO;0x1301bf;;;AU)(A;;0x4;;;AU)" },
         { L"authenticated-users-modify-inherited-fails", AdminUpdateChecks::PlaceInside, false, L"S-1-5-11", false, true, L"O:BAD:AI(A;ID;FA;;;BA)(A;ID;FA;;;SY)(A;OICIID;0x1301bf;;;AU)(A;ID;0x1200a9;;;BU)" },
         { L"users-write-on-a-dll-fails", AdminUpdateChecks::PlaceInside, false, L"S-1-5-32-545", false, false, L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1301bf;;;BU)" },
         { L"the-service-modify-on-bin-fails", AdminUpdateChecks::PlaceInside, false, L"S-1-5-80-3504853920-2627644491-1723263464-2546768561-1700046809", false, false, L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;OICI;0x1301bf;;;S-1-5-80-3504853920-2627644491-1723263464-2546768561-1700046809)" },
         { L"a-user-as-owner-fails", AdminUpdateChecks::PlaceInside, false, L"S-1-5-21-1004336348-1177238915-682003330-1001", true, false, L"O:S-1-5-21-1004336348-1177238915-682003330-1001D:(A;;FA;;;SY)(A;;FA;;;BA)" },
         { L"a-user-as-owner-of-an-ancestor-fails", AdminUpdateChecks::PlaceAboveApp, false, L"S-1-5-21-1004336348-1177238915-682003330-1001", true, false, L"O:S-1-5-21-1004336348-1177238915-682003330-1001D:(A;;FA;;;SY)(A;;FA;;;BA)" },
         { L"creator-owner-inherit-only-passes", AdminUpdateChecks::PlaceInside, true, L"", false, false, L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;OICIIO;GA;;;CO)" },
         { L"creator-owner-effective-fails", AdminUpdateChecks::PlaceInside, false, L"S-1-3-0", false, false, L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;CO)" },
         { L"users-delete-child-on-an-ancestor-fails", AdminUpdateChecks::PlaceAboveApp, false, L"S-1-5-32-545", false, false, L"O:SYD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x40;;;BU)" },
         { L"users-delete-on-an-ancestor-fails", AdminUpdateChecks::PlaceAboveApp, false, L"S-1-5-32-545", false, false, L"O:SYD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;SD;;;BU)" },
         { L"users-create-files-and-folders-on-an-ancestor-passes", AdminUpdateChecks::PlaceAboveApp, true, L"", false, false, L"O:SYD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200af;;;BU)" },
         { L"users-create-files-in-the-installation-fails", AdminUpdateChecks::PlaceInside, false, L"S-1-5-32-545", false, false, L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200af;;;BU)" },
         { L"a-deny-entry-is-ignored", AdminUpdateChecks::PlaceInside, true, L"", false, false, L"O:BAD:(D;;FA;;;BU)(A;;FA;;;SY)(A;;FA;;;BA)" },
         { L"a-deny-entry-does-not-take-back-a-grant", AdminUpdateChecks::PlaceInside, false, L"S-1-5-32-545", false, false, L"O:BAD:(D;;FA;;;BU)(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1301bf;;;BU)" },
         { L"write-dac-alone-fails-inside", AdminUpdateChecks::PlaceInside, false, L"S-1-5-32-545", false, false, L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;WD;;;BU)" },
         { L"write-dac-alone-fails-above", AdminUpdateChecks::PlaceAboveApp, false, L"S-1-5-32-545", false, false, L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;WD;;;BU)" },
         { L"write-owner-alone-fails-above", AdminUpdateChecks::PlaceAboveApp, false, L"S-1-5-32-545", false, false, L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;WO;;;BU)" },
         { L"generic-write-fails-inside", AdminUpdateChecks::PlaceInside, false, L"S-1-5-32-545", false, false, L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;GW;;;BU)" },
         { L"generic-all-fails-above", AdminUpdateChecks::PlaceAboveApp, false, L"S-1-1-0", false, false, L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;GA;;;WD)" },
         { L"write-attributes-fails-inside", AdminUpdateChecks::PlaceInside, false, L"S-1-5-32-545", false, false, L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x100;;;BU)" },
         { L"write-attributes-passes-above", AdminUpdateChecks::PlaceAboveApp, true, L"", false, false, L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x100;;;BU)" },
         { L"no-dacl-at-all-fails", AdminUpdateChecks::PlaceInside, false, L"S-1-1-0", false, false, L"O:BAD:NO_ACCESS_CONTROL" },
         { L"trustedinstaller-full-control-passes", AdminUpdateChecks::PlaceInside, true, L"", false, false, L"O:S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464D:(A;;FA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)(A;;FA;;;SY)" },
         { L"everyone-read-passes", AdminUpdateChecks::PlaceInside, true, L"", false, false, L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;WD)" },
         { L"system-as-owner-passes", AdminUpdateChecks::PlaceInside, true, L"", false, false, L"O:SYD:(A;;FA;;;SY)(A;;FA;;;BA)" },
         { L"owner-rights-write-fails", AdminUpdateChecks::PlaceInside, false, L"S-1-3-4", false, false, L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1301bf;;;S-1-3-4)" },
         { L"users-write-inherit-only-passes", AdminUpdateChecks::PlaceInside, true, L"", false, false, L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;OICIIO;0x1301bf;;;BU)" },
         { L"adminupdate-as-the-stage-makes-it-passes", AdminUpdateChecks::PlaceInside, true, L"", false, false, L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;;RC;;;AU)" },
      };
      // PERMISSION-CASES-END
   }

   AdminUpdateTester::AdminUpdateTester(bool allowUnelevated) :
      allowUnelevated_(allowUnelevated),
      elevated_(false)
   {
   }

   std::wstring
   AdminUpdateTester::FixturesFolder()
   {
      wchar_t buffer[MAX_PATH];
      const DWORD length = GetEnvironmentVariableW(L"HMAILSERVER_INSTALLER_FIXTURES", buffer, MAX_PATH);
      if (length > 0 && length < MAX_PATH)
         return std::wstring(buffer, length);
      return L"C:\\HMTest\\Installers";
   }

   void
   AdminUpdateTester::Test()
   {
      elevated_ = IsElevated_();
      skipped_.clear();

      wchar_t temp[MAX_PATH];
      const DWORD length = GetTempPathW(MAX_PATH, temp);
      Expect_(length > 0 && length < MAX_PATH, L"the temp folder could not be named");
      scratch_ = Join_(std::wstring(temp, length), L"hm-admin-update-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
      Expect_(CreateDirectoryW(scratch_.c_str(), nullptr) != FALSE, L"the scratch folder could not be made: " + scratch_);

      try
      {
         TestPatterns_();
         TestPermissionRule_();
         TestWorkFolder_();
         TestSafeCopy_();
         TestArchitectureAndVersions_();
         TestPinAlone_();
         TestAuthenticode_();
         TestLoweredChild_();
      }
      catch (...)
      {
         RemoveTree_(scratch_);
         throw;
      }
      RemoveTree_(scratch_);
   }

   void
   AdminUpdateTester::TestPatterns_()
   {
      using namespace AdminUpdateChecks;

      Expect_(IsWorkId(L"0123456789abcdef0123456789abcdef"), L"a work id of 32 lowercase hex characters was refused");
      Expect_(!IsWorkId(L"0123456789abcdef0123456789abcde"), L"a work id of 31 characters was accepted");
      Expect_(!IsWorkId(L"0123456789ABCDEF0123456789abcdef"), L"a work id in upper case was accepted");
      Expect_(!IsWorkId(L"0123456789abcdef0123456789abcde\\"), L"a work id with a backslash was accepted");
      Expect_(!IsWorkId(L"..\\..\\..\\Windows\\System32\\x"), L"a path was accepted as a work id");

      Expect_(IsReleaseVersion(L"6.3.7") && IsReleaseVersion(L"6.3.7.1") && IsReleaseVersion(L"10.0") && IsReleaseVersion(L"6.3.7-rc1") &&
              IsReleaseVersion(L"6.3.7-B45.2"), L"a release version was refused");
      Expect_(!IsReleaseVersion(L"6.3.7 & calc"), L"\"6.3.7 & calc\" was accepted as a version");
      Expect_(!IsReleaseVersion(L"6") && !IsReleaseVersion(L"6.3.7.1.2") && !IsReleaseVersion(L"v6.3.7") && !IsReleaseVersion(L"6..3") &&
              !IsReleaseVersion(L".6.3") && !IsReleaseVersion(L"6.3.") && !IsReleaseVersion(L"6.3.7-") && !IsReleaseVersion(L"6.3.7-a_b") &&
              !IsReleaseVersion(L"6.3.7-abcdefghijklmnopqrstu") && !IsReleaseVersion(L"6.3.7\"") && !IsReleaseVersion(L"") &&
              !IsReleaseVersion(L"\xFF16.3.7"), L"a malformed version was accepted");

      const std::string token(64, 'a');
      Expect_(IsToken(token) && IsToken("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"), L"a token was refused");
      Expect_(!IsToken(std::string(63, 'a')) && !IsToken(std::string(65, 'a')), L"a token of 63 or 65 characters was accepted");
      Expect_(!IsToken(std::string(64, 'A')) && !IsToken(std::string(63, 'a') + "g") && !IsToken(std::string(63, 'a') + "\n"),
              L"a token in upper case or with a character that is not hex was accepted");

      unsigned long value = 0;
      Expect_(IsDecimal(L"4242", value) && value == 4242 && IsDecimal(L"4294967295", value) && value == 4294967295ul,
              L"a decimal was refused or misread");
      Expect_(!IsDecimal(L"4294967296", value) && !IsDecimal(L"12a", value) && !IsDecimal(L"-1", value) && !IsDecimal(L"", value) &&
              !IsDecimal(L" 1", value), L"a malformed decimal was accepted");
   }

   void
   AdminUpdateTester::TestPermissionRule_()
   {
      using namespace AdminUpdateChecks;

      for (const PermissionCase_ &item : PermissionCases_)
      {
         PSECURITY_DESCRIPTOR descriptor = nullptr;
         ULONG size = 0;
         Expect_(ConvertStringSecurityDescriptorToSecurityDescriptorW(item.sddl, SDDL_REVISION_1, &descriptor, &size) != FALSE,
                 std::wstring(L"the SDDL of ") + item.name + L" does not parse");
         Finding finding;
         const bool passed = EvaluateDescriptor(descriptor, item.place, finding);
         LocalFree(descriptor);

         Expect_(passed == item.pass, std::wstring(item.name) + (item.pass ? L" failed: " + finding.principal + L" may " + finding.right + finding.reason : L" passed"));
         if (!item.pass)
         {
            Expect_(_wcsicmp(finding.sid.c_str(), item.sid) == 0, std::wstring(item.name) + L" named " + finding.sid + L", not " + item.sid);
            Expect_(finding.owner == item.owner, std::wstring(item.name) + L" said the owner " + (finding.owner ? L"was" : L"was not") + L" at fault");
            Expect_(finding.inherited == item.inherited, std::wstring(item.name) + L" said the entry " + (finding.inherited ? L"was" : L"was not") + L" inherited");
            Expect_(!finding.principal.empty() && !finding.right.empty(), std::wstring(item.name) + L" named no principal or right");
         }
      }

      // A conditional grant counts as a grant: a condition could be true.
      {
         PSECURITY_DESCRIPTOR descriptor = nullptr;
         ULONG size = 0;
         Expect_(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(XA;;0x1301bf;;;BU;(Member_of {SID(BA)}))",
                                                                      SDDL_REVISION_1, &descriptor, &size) != FALSE, L"the conditional SDDL does not parse");
         Finding finding;
         const bool passed = EvaluateDescriptor(descriptor, PlaceInside, finding);
         LocalFree(descriptor);
         Expect_(!passed && finding.sid == L"S-1-5-32-545", L"a conditional grant of Modify to Users passed");
      }

      // The rule on a real object: this tester's own scratch folder is under the
      // user's profile (or SYSTEM's), owned by that user, which is not trusted -
      // and CheckObject names it with the icacls line that would set the owner.
      if (!elevated_)
      {
         Finding finding;
         Expect_(!CheckObject(scratch_, PlaceInside, scratch_, finding) && finding.owner && finding.fix.find(L"/setowner *S-1-5-32-544") != std::wstring::npos,
                 L"a folder owned by an unelevated user passed, or its fix does not set the owner: " + finding.fix);
      }
   }

   void
   AdminUpdateTester::TestWorkFolder_()
   {
      using namespace AdminUpdateChecks;

      if (!elevated_ && !allowUnelevated_)
         Expect_(false, L"the work folder can only be tested with an administrator's token (the internal suite runs as SYSTEM)");

      // An administrator's token makes the Administrators the owner, as the stage
      // does; an unelevated run makes itself the owner, and then only what does
      // not depend on the owner is checked.
      const std::wstring owner = elevated_ ? std::wstring() : CurrentUserSid();
      if (!elevated_)
         skipped_.push_back(L"work folder: the owner is the Administrators only when elevated; checked the rest with this user as owner");

      // Unelevated, this user reads the folders it makes, or it could not hold
      // them: the stage, an administrator, is given no such entry.
      std::vector<std::wstring> selfReader;
      if (!elevated_)
         selfReader.push_back(CurrentUserSid());

      const std::wstring adminUpdate = Join_(scratch_, L"AdminUpdate");
      HANDLE parent = INVALID_HANDLE_VALUE;
      std::wstring why;
      Expect_(MakeOrOpenAdminFolder(adminUpdate, false, owner, parent, why, selfReader, true), L"AdminUpdate could not be made: " + why);

      // Unelevated, nobody but an administrator may add to AdminUpdate - which is
      // the point - so the work folder is made beside it.
      const std::wstring workParent = elevated_ ? adminUpdate : scratch_;
      const std::wstring work = Join_(workParent, L"0123456789abcdef0123456789abcdef");
      HANDLE held = INVALID_HANDLE_VALUE;
      Expect_(MakeOrOpenAdminFolder(work, true, owner, held, why, selfReader), L"the work folder could not be made: " + why);

      // Its descriptor as made: a protected DACL of exactly SYSTEM and the
      // Administrators, and the owner asked for.
      {
         std::vector<unsigned char> descriptor;
         Expect_(ReadDescriptor(work, descriptor, why), L"the work folder's descriptor could not be read: " + why);
         SECURITY_DESCRIPTOR_CONTROL control = 0;
         DWORD revision = 0;
         GetSecurityDescriptorControl((PSECURITY_DESCRIPTOR) descriptor.data(), &control, &revision);
         Expect_((control & SE_DACL_PROTECTED) != 0, L"the work folder's DACL is not protected from inheritance");
         BOOL present = FALSE, defaulted = FALSE;
         PACL dacl = nullptr;
         GetSecurityDescriptorDacl((PSECURITY_DESCRIPTOR) descriptor.data(), &present, &dacl, &defaulted);
         Expect_(present && dacl != nullptr && dacl->AceCount == 2 + selfReader.size(), L"the work folder's DACL is not exactly SYSTEM and the Administrators");
         for (DWORD i = 0; i < dacl->AceCount; i++)
         {
            LPVOID entry = nullptr;
            GetAce(dacl, i, &entry);
            const ACCESS_ALLOWED_ACE *allow = (const ACCESS_ALLOWED_ACE *) entry;
            const bool reader = !selfReader.empty() && TextOfSid((PSID) &allow->SidStart) == selfReader[0] &&
                                (allow->Mask & (FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD)) == 0;
            Expect_(allow->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && (IsTrustedSid((PSID) &allow->SidStart) || reader),
                    L"the work folder grants someone other than SYSTEM and the Administrators");
            if (IsTrustedSid((PSID) &allow->SidStart))
               Expect_((allow->Header.AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) == (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE),
                       L"the work folder does not hand SYSTEM's and the Administrators' entries down");
         }
         PSID ownerSid = nullptr;
         GetSecurityDescriptorOwner((PSECURITY_DESCRIPTOR) descriptor.data(), &ownerSid, &defaulted);
         if (elevated_)
            Expect_(IsWellKnownSid(ownerSid, WinBuiltinAdministratorsSid), L"the work folder's owner is not the Administrators");
         else
            Expect_(TextOfSid(ownerSid) == owner, L"the work folder's owner is not the one asked for");
      }

      // AdminUpdate as the stage makes it: the same protected entries, and
      // Authenticated Users may read its permissions - READ_CONTROL, on the folder
      // itself - and nothing else, so that the Control Panel can hold it to the
      // rule unelevated before the prompt of every install after the first. A
      // token that holds Authenticated Users alone reads AdminUpdate's
      // permissions, and not the work folder's, which is made without that entry.
      {
         std::vector<unsigned char> descriptor;
         Expect_(ReadDescriptor(adminUpdate, descriptor, why), L"AdminUpdate's descriptor could not be read: " + why);
         SECURITY_DESCRIPTOR_CONTROL control = 0;
         DWORD revision = 0;
         GetSecurityDescriptorControl((PSECURITY_DESCRIPTOR) descriptor.data(), &control, &revision);
         Expect_((control & SE_DACL_PROTECTED) != 0, L"AdminUpdate's DACL is not protected from inheritance");
         BOOL present = FALSE, defaulted = FALSE;
         PACL dacl = nullptr;
         GetSecurityDescriptorDacl((PSECURITY_DESCRIPTOR) descriptor.data(), &present, &dacl, &defaulted);
         Expect_(present && dacl != nullptr && dacl->AceCount == 3 + selfReader.size(),
                 L"AdminUpdate's DACL is not SYSTEM, the Administrators and Authenticated Users reading its permissions");
         bool readable = false;
         for (DWORD i = 0; i < dacl->AceCount; i++)
         {
            LPVOID entry = nullptr;
            GetAce(dacl, i, &entry);
            const ACCESS_ALLOWED_ACE *allow = (const ACCESS_ALLOWED_ACE *) entry;
            Expect_(allow->Header.AceType == ACCESS_ALLOWED_ACE_TYPE, L"AdminUpdate's DACL holds an entry that is not a grant");
            const std::wstring sid = TextOfSid((PSID) &allow->SidStart);
            if (sid == L"S-1-5-11")
            {
               readable = true;
               Expect_(allow->Mask == READ_CONTROL && (allow->Header.AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) == 0,
                       L"Authenticated Users may do more with AdminUpdate than read its permissions, or pass that on to what is made in it");
            }
            else
               Expect_(IsTrustedSid((PSID) &allow->SidStart) || (!selfReader.empty() && sid == selfReader[0]),
                       L"AdminUpdate grants someone other than SYSTEM, the Administrators and Authenticated Users");
         }
         Expect_(readable, L"Authenticated Users may not read AdminUpdate's permissions");

         HANDLE limited = AuthenticatedUsersOnlyToken_();
         Expect_(limited != nullptr, L"a token holding Authenticated Users alone could not be made (Windows error " + std::to_wstring(GetLastError()) + L")");
         DWORD adminUpdateError = 0, workError = 0;
         const bool readsAdminUpdate = CanReadPermissionsAs_(limited, adminUpdate, adminUpdateError);
         const bool readsWork = CanReadPermissionsAs_(limited, work, workError);
         CloseHandle(limited);
         Expect_(!readsWork, L"a token holding Authenticated Users alone read the work folder's permissions, so it cannot show who may read AdminUpdate's");
         Expect_(readsAdminUpdate, L"a token holding Authenticated Users alone - an unelevated Control Panel - could not read AdminUpdate's permissions (Windows error " +
                                       std::to_wstring(adminUpdateError) + L"), so every install after the first would be refused before the prompt");
      }

      // A second folder of the same name is refused: the stage's names are new.
      HANDLE again = INVALID_HANDLE_VALUE;
      Expect_(!MakeOrOpenAdminFolder(work, true, owner, again, why, selfReader) && why.find(L"already there") != std::wstring::npos,
              L"a work folder that was already there was accepted as new");

      // A held folder cannot be renamed, and neither can the one above it.
      Expect_(!MoveFileW(work.c_str(), (work + L"-moved").c_str()), L"the held work folder could be renamed");
      Expect_(!MoveFileW(workParent.c_str(), (workParent + L"-moved").c_str()), L"the folder above the held work folder could be renamed");

      // A file in it is made new, and a second of the same name is refused. An
      // unelevated run may not add a file to a folder only administrators may
      // write, which is the point of it, so it makes the file beside it.
      const std::wstring statusPath = Join_(elevated_ ? work : scratch_, L"status.txt");
      std::vector<std::wstring> readers;
      readers.push_back(CurrentUserSid());
      HANDLE status = CreateAdminFile(statusPath, readers, FILE_GENERIC_READ, owner, why);
      Expect_(status != INVALID_HANDLE_VALUE, L"status.txt could not be made: " + why);
      HANDLE second = CreateAdminFile(statusPath, readers, FILE_GENERIC_READ, owner, why);
      Expect_(second == INVALID_HANDLE_VALUE, L"a file made new was made a second time over the first");
      if (second != INVALID_HANDLE_VALUE)
         CloseHandle(second);
      {
         std::vector<unsigned char> descriptor;
         Expect_(ReadDescriptor(statusPath, descriptor, why), L"status.txt's descriptor could not be read: " + why);
         BOOL present = FALSE, defaulted = FALSE;
         PACL dacl = nullptr;
         GetSecurityDescriptorDacl((PSECURITY_DESCRIPTOR) descriptor.data(), &present, &dacl, &defaulted);
         Expect_(present && dacl != nullptr && dacl->AceCount == 3, L"status.txt does not carry SYSTEM, the Administrators and the reader alone");
         // The reader's entry is the one in its name that writes nothing - which
         // tells it from SYSTEM's own full control when the tester runs as SYSTEM,
         // as the internal suite does inside a LocalSystem service.
         bool readerFound = false;
         for (DWORD i = 0; i < dacl->AceCount; i++)
         {
            LPVOID entry = nullptr;
            GetAce(dacl, i, &entry);
            const ACCESS_ALLOWED_ACE *allow = (const ACCESS_ALLOWED_ACE *) entry;
            const bool writes = (allow->Mask & (FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER)) != 0;
            if (TextOfSid((PSID) &allow->SidStart) == readers[0] && !writes)
               readerFound = true;
            else
               Expect_(IsTrustedSid((PSID) &allow->SidStart), L"status.txt grants someone other than SYSTEM, the Administrators and its reader, or lets its reader write it");
         }
         Expect_(readerFound, L"status.txt has no entry for its reader");
      }
      CloseHandle(status);

      CloseHandle(held);
      CloseHandle(parent);

      // AdminUpdate already there as a junction is refused, whatever it leads to.
      {
         const std::wstring elsewhere = Join_(scratch_, L"elsewhere");
         CreateDirectoryW(elsewhere.c_str(), nullptr);
         const std::wstring linked = Join_(scratch_, L"AdminUpdateLinked");
         Expect_(MakeJunction_(linked, elsewhere), L"a junction could not be made for the test");
         HANDLE refused = INVALID_HANDLE_VALUE;
         Expect_(!MakeOrOpenAdminFolder(linked, false, owner, refused, why, selfReader) && (why.find(L"link") != std::wstring::npos || why.find(L"junction") != std::wstring::npos),
                 L"an AdminUpdate that is a junction was accepted: " + why);
         if (refused != INVALID_HANDLE_VALUE)
            CloseHandle(refused);
      }

      // One already there that others may write is refused.
      {
         const std::wstring open = Join_(scratch_, L"AdminUpdateOpen");
         PSECURITY_DESCRIPTOR descriptor = nullptr;
         ULONG size = 0;
         ConvertStringSecurityDescriptorToSecurityDescriptorW((L"O:" + (owner.empty() ? std::wstring(L"BA") : owner) + L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;BU)").c_str(),
                                                              SDDL_REVISION_1, &descriptor, &size);
         SECURITY_ATTRIBUTES attributes = { sizeof(attributes), descriptor, FALSE };
         Expect_(CreateDirectoryW(open.c_str(), &attributes) != FALSE, L"a folder others may write could not be made for the test");
         LocalFree(descriptor);
         HANDLE refused = INVALID_HANDLE_VALUE;
         Expect_(!MakeOrOpenAdminFolder(open, false, owner, refused, why, selfReader), L"an AdminUpdate that Users may write was accepted");
         if (refused != INVALID_HANDLE_VALUE)
            CloseHandle(refused);
      }
   }

   void
   AdminUpdateTester::TestSafeCopy_()
   {
      using namespace AdminUpdateChecks;

      const std::wstring updates = Join_(scratch_, L"Updates");
      Expect_(CreateDirectoryW(updates.c_str(), nullptr) != FALSE, L"the Updates folder could not be made for the test");
      const std::wstring source = Join_(updates, L"hMailServer-6.3.7-x64.exe");
      std::string content;
      for (int i = 0; i < 300000; i++)
         content += (char) ('a' + i % 26);
      Expect_(WriteFile_(source, content), L"the source could not be written");
      Expect_(WriteFile_(source + L":Zone.Identifier", "[ZoneTransfer]\r\nZoneId=3\r\n"), L"the source's alternate stream could not be written");

      auto destination = [&](const std::wstring &name, HANDLE &file)
      {
         std::wstring why;
         file = CreateFileW(Join_(scratch_, name).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
         Expect_(file != INVALID_HANDLE_VALUE, L"a destination could not be made for the test");
      };

      // A file, byte for byte, and not its alternate stream.
      {
         HANDLE copy = INVALID_HANDLE_VALUE;
         destination(L"copy-1.exe", copy);
         unsigned long long copied = 0;
         std::wstring why;
         Expect_(SafeCopy(source, source, 1ull << 30, copy, copied, why), L"a plain file was refused: " + why);
         CloseHandle(copy);
         Expect_(copied == content.size() && ReadAll_(Join_(scratch_, L"copy-1.exe")) == content, L"the copy is not the file byte for byte");
         WIN32_FIND_STREAM_DATA stream;
         HANDLE streams = FindFirstStreamW(Join_(scratch_, L"copy-1.exe").c_str(), FindStreamInfoStandard, &stream, 0);
         int count = 0;
         if (streams != INVALID_HANDLE_VALUE)
         {
            do
            {
               count++;
            } while (FindNextStreamW(streams, &stream));
            FindClose(streams);
         }
         Expect_(count == 1, L"the copy carries an alternate stream");
      }

      // The same bytes, sought through a path that is not where they are: refused.
      {
         HANDLE copy = INVALID_HANDLE_VALUE;
         destination(L"copy-2.exe", copy);
         unsigned long long copied = 0;
         std::wstring why;
         Expect_(!SafeCopy(source, Join_(updates, L"other.exe"), 1ull << 30, copy, copied, why) && copied == 0,
                 L"a file was copied although it is not at the path expected");
         CloseHandle(copy);
      }

      // A folder, and a file over the cap: refused.
      {
         HANDLE copy = INVALID_HANDLE_VALUE;
         destination(L"copy-3.exe", copy);
         unsigned long long copied = 0;
         std::wstring why;
         Expect_(!SafeCopy(updates, updates, 1ull << 30, copy, copied, why), L"a folder was copied as a file");
         Expect_(!SafeCopy(source, source, 1000, copy, copied, why) && why.find(L"more than") != std::wstring::npos, L"a file over the cap was copied");
         CloseHandle(copy);
      }

      // A junction on the way: Updates made a junction to another folder, the file
      // sought by the path through it. Windows resolves the handle to the real
      // folder, which is not the path the stage expects.
      {
         const std::wstring real = Join_(scratch_, L"Planted");
         CreateDirectoryW(real.c_str(), nullptr);
         Expect_(WriteFile_(Join_(real, L"hMailServer-6.3.7-x64.exe"), "planted"), L"the planted file could not be written");
         const std::wstring linked = Join_(scratch_, L"UpdatesLinked");
         Expect_(MakeJunction_(linked, real), L"a junction could not be made for the test");
         const std::wstring through = Join_(linked, L"hMailServer-6.3.7-x64.exe");
         HANDLE copy = INVALID_HANDLE_VALUE;
         destination(L"copy-4.exe", copy);
         unsigned long long copied = 0;
         std::wstring why;
         Expect_(!SafeCopy(through, through, 1ull << 30, copy, copied, why) && why.find(L"resolves to") != std::wstring::npos,
                 L"a file reached through a junction was copied: " + why);
         CloseHandle(copy);
      }

      // A symbolic link in the Updates folder, where this account may make one
      // (an administrator, or Developer Mode): refused as a link.
      {
         const std::wstring link = Join_(updates, L"linked.exe");
         if (CreateSymbolicLinkW(link.c_str(), source.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
         {
            HANDLE copy = INVALID_HANDLE_VALUE;
            destination(L"copy-5.exe", copy);
            unsigned long long copied = 0;
            std::wstring why;
            Expect_(!SafeCopy(link, link, 1ull << 30, copy, copied, why) && why.find(L"link") != std::wstring::npos,
                    L"a symbolic link was copied: " + why);
            CloseHandle(copy);
         }
         else
            skipped_.push_back(L"safe copy: this account may not make a symbolic link, so a link in the Updates folder was not tried");
      }

      // A file held for reading cannot be written, renamed or deleted meanwhile.
      {
         std::wstring why;
         HANDLE held = HoldForReading(source, why);
         Expect_(held != INVALID_HANDLE_VALUE, L"a file could not be held: " + why);
         HANDLE writer = CreateFileW(source.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
         Expect_(writer == INVALID_HANDLE_VALUE, L"a held file could be opened for writing");
         Expect_(!MoveFileW(source.c_str(), (source + L".moved").c_str()), L"a held file could be renamed");
         Expect_(!DeleteFileW(source.c_str()), L"a held file could be deleted");
         const std::string digest = Sha256Of(held, why);
         Expect_(digest.size() == 64, L"the SHA-256 of a held file could not be read: " + why);
         CloseHandle(held);
      }
   }

   void
   AdminUpdateTester::TestArchitectureAndVersions_()
   {
      using namespace AdminUpdateChecks;

      Expect_(ArchitectureFromDescription(L"hMailServer Setup") == ArchitectureX64, L"\"hMailServer Setup\" is not read as x64");
      Expect_(ArchitectureFromDescription(L"hMailServer Setup (x64)") == ArchitectureX64, L"\"(x64)\" is not read as x64");
      Expect_(ArchitectureFromDescription(L"hMailServer Setup (ARM64)") == ArchitectureArm64, L"\"(ARM64)\" is not read as ARM64");
      Expect_(ArchitectureFromDescription(L"hMailServer Setup (x86)") == ArchitectureOther, L"\"(x86)\" is not read as another architecture");
      Expect_(ArchitectureFromDescription(L"") == ArchitectureX64, L"an empty description is not read as x64");

      Expect_(CompareVersions(L"6.3.7", L"6.3.6") > 0 && CompareVersions(L"6.3.6", L"6.3.6") == 0 && CompareVersions(L"6.3.5", L"6.3.6") < 0,
              L"the version order is wrong");
      Expect_(CompareVersions(L"6.10.0", L"6.9.9") > 0 && CompareVersions(L"6.3.7.0", L"6.3.7") == 0 && CompareVersions(L"6.3.7-rc1", L"6.3.7") == 0,
              L"the version order is wrong for more digits, a fourth component or a suffix");
   }

   void
   AdminUpdateTester::TestPinAlone_()
   {
      using namespace AuthenticodePublisher;

      Signer genuine;
      genuine.subject = ProjectSubject();
      genuine.subjectText = L"CN=Progressive Robot Ltd, O=Progressive Robot Ltd, L=Chester, S=Cheshire West and Chester, C=GB";
      genuine.chainSha256.push_back("00");
      genuine.chainSha256.push_back("11");
      genuine.chainSha256.push_back(IdVerifiedCodeSigningPca2021);
      genuine.chainSha256.push_back(IdentityVerificationRoot2020);
      genuine.chainNames.assign(4, L"x");
      genuine.ekus.push_back("1.3.6.1.4.1.311.97.1.0");
      genuine.ekus.push_back("1.3.6.1.5.5.7.3.3");
      genuine.timestamped = true;

      std::wstring why;
      Expect_(EvaluatePin(genuine, ProjectPin(), why) == Passed, L"the pin refused the project's own signer: " + why);

      Signer otherRoot = genuine;
      otherRoot.chainSha256[3] = "22";
      Expect_(EvaluatePin(otherRoot, ProjectPin(), why) == WrongChain, L"the pin passed a chain to another root");

      Signer otherPca = genuine;
      otherPca.chainSha256[2] = "33";
      Expect_(EvaluatePin(otherPca, ProjectPin(), why) == WrongChain, L"the pin passed a chain through another PCA");

      Signer shortChain = genuine;
      shortChain.chainSha256.erase(shortChain.chainSha256.begin() + 1);
      Expect_(EvaluatePin(shortChain, ProjectPin(), why) == WrongChain, L"the pin passed a chain of three");

      Signer extra = genuine;
      extra.subject.push_back(NameAttribute("2.5.4.5", L"15400700"));
      Expect_(EvaluatePin(extra, ProjectPin(), why) == WrongPublisher, L"the pin passed a subject with an attribute more");

      Signer fewer = genuine;
      fewer.subject.pop_back();
      Expect_(EvaluatePin(fewer, ProjectPin(), why) == WrongPublisher, L"the pin passed a subject with an attribute fewer");

      Signer lookalike = genuine;
      lookalike.subject[0].second = L"Progressive Robot Ltd.";
      Expect_(EvaluatePin(lookalike, ProjectPin(), why) == WrongPublisher, L"the pin passed a look-alike common name");

      Signer noCodeSigning = genuine;
      noCodeSigning.ekus.pop_back();
      Expect_(EvaluatePin(noCodeSigning, ProjectPin(), why) == NoCodeSigning, L"the pin passed a certificate without the code-signing EKU");

      Signer untimed = genuine;
      untimed.timestamped = false;
      Expect_(EvaluatePin(untimed, ProjectPin(), why) == NotTimestamped, L"the pin passed a signature without a timestamp");

      // The site's own signer: the installed program's subject, through the
      // installed program's root.
      Signer site;
      site.subject.push_back(NameAttribute("2.5.4.3", L"Example Mail Ltd"));
      site.subject.push_back(NameAttribute("2.5.4.6", L"GB"));
      site.subjectText = L"CN=Example Mail Ltd, C=GB";
      site.chainSha256.push_back("aa");
      site.chainSha256.push_back("bb");
      site.chainNames.push_back(L"Example Mail Ltd");
      site.chainNames.push_back(L"Example Root");
      site.ekus.push_back("1.3.6.1.5.5.7.3.3");
      site.timestamped = true;

      Pin samePin;
      samePin.sameAsInstalled = true;
      samePin.installedSubject = site.subject;
      samePin.installedSubjectText = site.subjectText;
      samePin.installedRootSha256 = "bb";
      Expect_(EvaluatePin(site, samePin, why) == Passed, L"the site-signer pin refused the installed program's own signer: " + why);
      Expect_(EvaluatePin(genuine, samePin, why) == WrongPublisher, L"the site-signer pin passed another publisher");

      // The same name through another root this machine trusts is another signer.
      Signer sameNameOtherRoot = site;
      sameNameOtherRoot.chainSha256[1] = "cc";
      Expect_(EvaluatePin(sameNameOtherRoot, samePin, why) == WrongChain, L"the site-signer pin passed the installed signer's name through another root");
      Pin noRoot = samePin;
      noRoot.installedRootSha256.clear();
      Expect_(EvaluatePin(site, noRoot, why) == WrongChain, L"the site-signer pin passed with no root of the installed program's to hold it to");

      Pin unsignedPin;
      unsignedPin.sameAsInstalled = true;
      Expect_(EvaluatePin(site, unsignedPin, why) == NothingToPinTo, L"the site-signer pin passed with an unsigned installed program");

      // A subject is read whole: the project's name with a NUL and more after it
      // is not the project's name, and is not read at all. Windows' own decoder
      // refuses such a name already (a UTF8String, T61String, IA5String,
      // BMPString or UniversalString with a NUL, tried 1 October 2026), so this
      // holds the outcome; AttributesOfName's whole-length reading is what keeps
      // it should a decoder ever hand one over.
      {
         auto encode = [](const std::string &value, DWORD type, std::vector<unsigned char> &der) -> bool
         {
            CERT_RDN_ATTR attribute;
            memset(&attribute, 0, sizeof(attribute));
            attribute.pszObjId = const_cast<LPSTR>("2.5.4.3");
            attribute.dwValueType = type;
            attribute.Value.pbData = (BYTE *) value.data();
            attribute.Value.cbData = (DWORD) value.size();
            CERT_RDN rdn = { 1, &attribute };
            CERT_NAME_INFO info = { 1, &rdn };
            DWORD size = 0;
            // X509_NAME is wincrypt's structure type 7 (AuthenticodePublisher.cpp).
            if (!CryptEncodeObjectEx(X509_ASN_ENCODING, (LPCSTR) 7, &info, 0, nullptr, nullptr, &size) || size == 0)
               return false;
            der.assign(size, 0);
            return CryptEncodeObjectEx(X509_ASN_ENCODING, (LPCSTR) 7, &info, 0, nullptr, der.data(), &size) != FALSE;
         };
         std::vector<unsigned char> der;
         std::vector<NameAttribute> attributes;
         Expect_(encode("Progressive Robot Ltd", CERT_RDN_T61_STRING, der), L"a name could not be encoded for the test");
         Expect_(AttributesOfName(der.data(), (unsigned long) der.size(), attributes) && attributes.size() == 1 && attributes[0].first == "2.5.4.3" &&
                 attributes[0].second == L"Progressive Robot Ltd", L"a plain common name was not read back");
         Expect_(encode(std::string("Progressive Robot Ltd") + std::string(1, '\0') + ".example", CERT_RDN_T61_STRING, der),
                 L"a name with a NUL could not be encoded for the test");
         Expect_(!AttributesOfName(der.data(), (unsigned long) der.size(), attributes) && attributes.empty(),
                 L"a common name with a NUL in it was read, or read as far as the NUL");
      }

      // What WinVerifyTrust's answers mean: revoked is not offline, offline is
      // not passed.
      Expect_(OutcomeOfStatus(ERROR_SUCCESS) == Passed && OutcomeOfStatus((long) CERT_E_REVOKED) == Revoked &&
              OutcomeOfStatus((long) CRYPT_E_REVOCATION_OFFLINE) == RevocationOffline && OutcomeOfStatus((long) CRYPT_E_NO_REVOCATION_CHECK) == RevocationOffline &&
              OutcomeOfStatus((long) TRUST_E_BAD_DIGEST) == BadDigest && OutcomeOfStatus((long) TRUST_E_NOSIGNATURE) == NotSigned &&
              OutcomeOfStatus((long) CERT_E_UNTRUSTEDROOT) == Untrusted && OutcomeOfStatus((long) CERT_E_EXPIRED) == Untrusted,
              L"a WinVerifyTrust answer is misread");

      // And what the chain it built says: revocation established where no
      // certificate but the root says its status is unknown or offline - the
      // answer WinVerifyTrust itself takes as good (on the installer smoke VM on
      // 1 October 2026 a revocation server that never answered, and the
      // installer accepted with "revocation checked online").
      {
         const unsigned long offline = CERT_TRUST_REVOCATION_STATUS_UNKNOWN | CERT_TRUST_IS_OFFLINE_REVOCATION;
         Expect_(RevocationEstablished({ 0, 0, 0, 0 }) && RevocationEstablished({ 0, offline }) && RevocationEstablished({ 0 }) &&
                 RevocationEstablished({ CERT_TRUST_IS_NOT_TIME_VALID, 0 }),
                 L"a chain whose revocation was established, or whose root alone was not checked, was not taken as established");
         Expect_(!RevocationEstablished({ offline, 0 }) && !RevocationEstablished({ 0, CERT_TRUST_REVOCATION_STATUS_UNKNOWN, 0, 0 }) &&
                 !RevocationEstablished({ CERT_TRUST_IS_OFFLINE_REVOCATION, 0 }) && !RevocationEstablished({ CERT_TRUST_IS_REVOKED, 0 }) &&
                 !RevocationEstablished({}),
                 L"a chain with a certificate whose revocation is unknown, offline or revoked was taken as established");
      }

      Expect_(HostOf(L"http://www.microsoft.com/pkiops/crl/x.crl") == L"www.microsoft.com" && HostOf(L"https://h:8080/x") == L"h" && HostOf(L"") == L"",
              L"the host of a CRL URL is misread");
   }

   void
   AdminUpdateTester::TestAuthenticode_()
   {
      using namespace AuthenticodePublisher;

      const std::wstring fixtures = FixturesFolder();
      auto fixture = [&](const std::wstring &name) -> std::wstring
      {
         const std::wstring path = Join_(fixtures, name);
         Expect_(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES,
                 L"the fixture " + path + L" is missing; run build/fetch-installer-fixtures.ps1, which fetches the real installers by their SHA-256 (or set HMAILSERVER_INSTALLER_FIXTURES)");
         return path;
      };

      auto verify = [&](const std::wstring &path, const Pin &pin, bool online, unsigned long timeout) -> Result
      {
         std::wstring why;
         HANDLE held = AdminUpdateChecks::HoldForReading(path, why);
         Expect_(held != INVALID_HANDLE_VALUE, L"a fixture could not be held: " + why);
         Result result;
         VerifyHeld(held, path, pin, online, timeout, result);
         CloseHandle(held);
         return result;
      };

      // The five signed releases: the same subject and root, three issuing CAs,
      // 6.3.1's leaf long expired and carried by its timestamp.
      const wchar_t *releases[] = { L"hMailServer-6.3.1-x64.exe", L"hMailServer-6.3.3-x64.exe", L"hMailServer-6.3.4-x64.exe", L"hMailServer-6.3.5-x64.exe" };
      for (const wchar_t *release : releases)
      {
         const Result result = verify(fixture(release), ProjectPin(), false, 120000);
         Expect_(result.outcome == Passed, std::wstring(release) + L" was refused: " + result.why);
         Expect_(result.signer.secondarySignatures == 0, std::wstring(release) + L" carries a secondary signature");
         Expect_(result.signer.chainSha256.size() == 4, std::wstring(release) + L"'s chain is not four certificates");
      }

      // 6.3.0 was not signed.
      {
         const Result result = verify(fixture(L"hMailServer-6.3.0-x64.exe"), ProjectPin(), false, 120000);
         Expect_(result.outcome == NotSigned, L"6.3.0, which is not signed, was not refused as unsigned: " + result.why);
      }

      const std::wstring genuine = fixture(L"hMailServer-6.3.5-x64.exe");

      // One byte of the setup data changed, in the middle of the file: the digest.
      {
         WIN32_FILE_ATTRIBUTE_DATA data;
         GetFileAttributesExW(genuine.c_str(), GetFileExInfoStandard, &data);
         const long long size = ((long long) data.nFileSizeHigh << 32) | data.nFileSizeLow;
         const std::wstring changed = Join_(scratch_, L"changed.exe");
         Expect_(CopyWithChange_(genuine, changed, size / 2, 0, std::string()), L"the changed copy could not be made");
         const Result result = verify(changed, ProjectPin(), false, 120000);
         Expect_(result.outcome == BadDigest, L"6.3.5 with one byte changed was not refused for its digest: " + result.why);
         DeleteFileW(changed.c_str());
      }

      // Cut short into its signature: refused.
      {
         const std::wstring cut = Join_(scratch_, L"cut.exe");
         Expect_(CopyWithChange_(genuine, cut, -1, 4096, std::string()), L"the truncated copy could not be made");
         const Result result = verify(cut, ProjectPin(), false, 120000);
         Expect_(result.outcome != Passed, L"6.3.5 cut short passed");
         DeleteFileW(cut.c_str());
      }

      // Bytes appended after the signature: refused, or - where Windows leaves the
      // tail out of the signed region - a file whose SHA-256 is not the release's,
      // which the Sigstore digest the stage checks refuses.
      {
         const std::wstring appended = Join_(scratch_, L"appended.exe");
         Expect_(CopyWithChange_(genuine, appended, -1, 0, std::string(4096, 'Z')), L"the appended copy could not be made");
         const Result result = verify(appended, ProjectPin(), false, 120000);
         if (result.outcome == Passed)
         {
            std::wstring why;
            HANDLE held = AdminUpdateChecks::HoldForReading(appended, why);
            const std::string digest = AdminUpdateChecks::Sha256Of(held, why);
            CloseHandle(held);
            Expect_(digest != "ebb708e056dfd1ca3bcb56b6356e819511b496965e0a82834bdf6987fec79e97",
                    L"6.3.5 with bytes appended has the release's own SHA-256");
            skipped_.push_back(L"authenticode: Windows accepts 6.3.5 with bytes appended after its signature; its SHA-256 differs from the release's, which the Sigstore digest refuses");
         }
         DeleteFileW(appended.c_str());
      }

      // A validly signed file of another publisher: Microsoft's .NET runtime.
      {
         std::wstring runtime;
         WIN32_FIND_DATAW data;
         HANDLE search = FindFirstFileW(Join_(fixtures, L"windowsdesktop-runtime-*-win-x64.exe").c_str(), &data);
         if (search != INVALID_HANDLE_VALUE)
         {
            runtime = Join_(fixtures, data.cFileName);
            FindClose(search);
         }
         Expect_(!runtime.empty(), L"the fixture windowsdesktop-runtime-*-win-x64.exe is missing from " + fixtures + L"; run build/fetch-installer-fixtures.ps1");
         const Result result = verify(runtime, ProjectPin(), false, 120000);
         Expect_(result.status == ERROR_SUCCESS, L"the .NET runtime installer is not validly signed, so it proves nothing: " + result.why);
         Expect_(result.outcome == WrongPublisher, L"Microsoft's .NET runtime installer was not refused as another publisher: " + result.why);
      }

      // A file Windows signs in a catalog, not in itself: not signed, as a file.
      {
         wchar_t system[MAX_PATH];
         GetSystemDirectoryW(system, MAX_PATH);
         const Result result = verify(Join_(system, L"cmd.exe"), ProjectPin(), false, 120000);
         Expect_(result.outcome == NotSigned, L"a catalog-signed Windows file was not refused as unsigned: " + result.why);
      }

      // The site-signer pin on a real signature: the installed program's subject
      // taken from 6.3.5 passes 6.3.4, and the .NET runtime's subject refuses it.
      {
         Signer installed;
         std::wstring why;
         Expect_(SignerOf(genuine, installed, why), L"the signer of 6.3.5 could not be read: " + why);
         Expect_(installed.subjectText == L"CN=Progressive Robot Ltd, O=Progressive Robot Ltd, L=Chester, S=Cheshire West and Chester, C=GB",
                 L"6.3.5's signer is read as " + installed.subjectText);
         Pin same;
         same.sameAsInstalled = true;
         same.installedSubject = installed.subject;
         same.installedSubjectText = installed.subjectText;
         same.installedRootSha256 = installed.chainSha256.empty() ? std::string() : installed.chainSha256.back();
         Expect_(same.installedRootSha256 == IdentityVerificationRoot2020, L"6.3.5's signature is not read as ending in Microsoft Identity Verification Root 2020");
         Result result = verify(fixture(L"hMailServer-6.3.4-x64.exe"), same, false, 120000);
         Expect_(result.outcome == Passed, L"the site-signer pin refused a release signed by the installed program's own signer: " + result.why);

         // The same subject held to another root: refused on a real signature too.
         Pin otherRoot = same;
         otherRoot.installedRootSha256 = IdVerifiedCodeSigningPca2021;
         result = verify(fixture(L"hMailServer-6.3.4-x64.exe"), otherRoot, false, 120000);
         Expect_(result.outcome == WrongChain, L"the site-signer pin passed a release whose chain ends in another root than the installed program's: " + result.why);

         Signer unsignedProgram;
         Expect_(!SignerOf(fixture(L"hMailServer-6.3.0-x64.exe"), unsignedProgram, why), L"an unsigned program was read as having a signer");
      }

      // A revocation check that is given no time is refused as timed out, not as
      // revoked, and not passed.
      {
         const Result result = verify(genuine, ProjectPin(), true, 1);
         Expect_(result.outcome == RevocationTimedOut, std::wstring(L"a revocation check given one millisecond was not refused as timed out: ") + OutcomeName(result.outcome));
      }

      // Online, as the stage checks: passed where the revocation server answers,
      // refused as unreachable where it does not - and never read as revoked.
      {
         const Result result = verify(genuine, ProjectPin(), true, 60000);
         Expect_(result.outcome == Passed || result.outcome == RevocationOffline || result.outcome == RevocationTimedOut,
                 std::wstring(L"6.3.5 checked online was refused as ") + OutcomeName(result.outcome) + L": " + result.why);
         if (result.outcome != Passed)
            skipped_.push_back(L"authenticode: the revocation server could not be reached from here (" + result.why + L")");
      }
   }

   void
   AdminUpdateTester::TestLoweredChild_()
   {
      wchar_t system[MAX_PATH];
      GetSystemDirectoryW(system, MAX_PATH);
      const std::wstring cmd = Join_(system, L"cmd.exe");

      // The groups UAC filters, which the child holds only to deny.
      {
         auto filtered = [](const wchar_t *text) -> bool
         {
            PSID sid = nullptr;
            if (!ConvertStringSidToSidW(text, &sid))
               Fail_(std::wstring(L"the SID ") + text + L" does not parse");
            const bool is = LoweredChild::IsFilteredByUac(sid);
            LocalFree(sid);
            return is;
         };
         Expect_(filtered(L"S-1-5-32-544") && filtered(L"S-1-5-32-551") && filtered(L"S-1-5-32-556") && filtered(L"S-1-5-114") &&
                 filtered(L"S-1-5-21-1004336348-1177238915-682003330-512") && filtered(L"S-1-5-21-1004336348-1177238915-682003330-519"),
                 L"a group UAC filters is not taken for one: the Administrators, Backup or Network Configuration Operators, a local administrator, Domain or Enterprise Admins");
         Expect_(!filtered(L"S-1-5-32-545") && !filtered(L"S-1-5-11") && !filtered(L"S-1-1-0") && !filtered(L"S-1-5-21-1004336348-1177238915-682003330-1001") &&
                 !filtered(L"S-1-5-21-1004336348-1177238915-682003330-513") && !filtered(L"S-1-5-32-578"),
                 L"a group UAC leaves alone is taken for one it filters: Users, Authenticated Users, Everyone, a user, Domain Users or Hyper-V Administrators");
      }

      // Its exit code, and its token read back before it runs.
      {
         LoweredChild::Request request;
         request.program = cmd;
         request.arguments = L"/d /c exit 7";
         request.timeoutMilliseconds = 30000;
         LoweredChild::TokenFacts facts;
         bool inspected = false;
         request.inspect = [&](HANDLE process)
         {
            HANDLE token = nullptr;
            if (OpenProcessToken(process, TOKEN_QUERY, &token))
            {
               inspected = LoweredChild::ReadTokenFacts(token, facts);
               CloseHandle(token);
            }
         };
         LoweredChild::Result result;
         Expect_(LoweredChild::Run(request, result), L"the lowered child could not be started: " + result.error);
         Expect_(inspected, L"the lowered child's token could not be read back");
         Expect_(!facts.administratorsPresent || (facts.administratorsDenyOnly && !facts.administratorsEnabled),
                 L"the lowered child's token has the Administrators enabled");
         Expect_(facts.enabledPrivileges.empty() || facts.enabledPrivileges == L"SeChangeNotifyPrivilege",
                 L"the lowered child's token has privileges enabled: " + facts.enabledPrivileges);
         Expect_(facts.enabledFilteredGroups.empty(), L"the lowered child's token has groups UAC filters enabled: " + facts.enabledFilteredGroups);
         Expect_(facts.integrityRid != 0 && facts.integrityRid <= SECURITY_MANDATORY_LOW_RID,
                 L"the lowered child runs above Low integrity (" + std::to_wstring(facts.integrityRid) + L")");
         Expect_(result.exitCode == 7 && !result.timedOut, L"the lowered child's exit code was not carried back: it ended with " + std::to_wstring(result.exitCode) +
                                                           (result.timedOut ? L", killed at its timeout" : L""));
      }

      // Its standard input and output, and nothing else.
      {
         LoweredChild::Request request;
         request.program = cmd;
         request.arguments = L"/d /v:on /c \"set /p line=& echo got !line!\"";
         request.input = "hello\r\n";
         request.timeoutMilliseconds = 30000;
         LoweredChild::Result result;
         Expect_(LoweredChild::Run(request, result), L"the lowered child could not be started: " + result.error);
         Expect_(result.output.find("got hello") != std::string::npos, L"the lowered child did not read its input and answer on its output");
      }

      // One process: a second is refused by its job.
      {
         LoweredChild::Request request;
         request.program = cmd;
         request.arguments = L"/d /c \"" + Join_(system, L"whoami.exe") + L"\" && echo second-started";
         request.timeoutMilliseconds = 30000;
         LoweredChild::Result result;
         Expect_(LoweredChild::Run(request, result), L"the lowered child could not be started: " + result.error);
         Expect_(result.output.find("second-started") == std::string::npos && result.exitCode != 0, L"the lowered child started a second process");
      }

      // One that does not answer in time is killed, and says so.
      {
         LoweredChild::Request request;
         request.program = cmd;
         request.arguments = L"/d /c \"for /l %i in (1,0,2) do rem\"";
         request.timeoutMilliseconds = 1500;
         LoweredChild::Result result;
         const ULONGLONG started = GetTickCount64();
         Expect_(LoweredChild::Run(request, result), L"the lowered child could not be started: " + result.error);
         Expect_(result.timedOut, L"a lowered child that never ends was not reported as timed out");
         Expect_(GetTickCount64() - started < 20000, L"a lowered child that never ends held its caller past its timeout");
      }

      // An answer longer than asked for is cut, and said to be.
      {
         LoweredChild::Request request;
         request.program = cmd;
         request.arguments = L"/d /c \"for /l %i in (1,1,20000) do @echo xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"";
         request.maximumOutputBytes = 1000;
         request.timeoutMilliseconds = 30000;
         LoweredChild::Result result;
         Expect_(LoweredChild::Run(request, result), L"the lowered child could not be started: " + result.error);
         Expect_(result.outputTruncated && result.output.size() == 1000, L"an answer past the most was not cut at it");
      }
   }
}

#endif
