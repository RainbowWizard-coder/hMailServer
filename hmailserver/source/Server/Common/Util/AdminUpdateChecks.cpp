// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#ifndef HM_PLATFORM_POSIX

#include "AdminUpdateChecks.h"

#include <aclapi.h>
#include <sddl.h>
#include <bcrypt.h>
#include <winver.h>

#include <algorithm>
#include <cwctype>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")

// Nothing in this file writes a log line, reports an error or reads a setting:
// it runs inside hMailServer.exe /InstallUpdate, elevated, before anything of the
// server's own is set up (build/check-elevated-verbs.py holds it to that).

namespace HM
{
   namespace AdminUpdateChecks
   {
      const wchar_t *const TrustedInstallerSid = L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

      const unsigned long WriteRights = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD |
                                        DELETE | WRITE_DAC | WRITE_OWNER;

      namespace
      {
         bool IsLowerHex_(wchar_t c)
         {
            return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
         }

         bool IsDigit_(wchar_t c)
         {
            return c >= L'0' && c <= L'9';
         }

         // The rights on a folder above {app} that let the next folder down be
         // replaced: removing a child, renaming this one, or changing who may.
         const DWORD ReplaceRights_ = FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER;

         const wchar_t *RightName_(DWORD mask)
         {
            if (mask & WRITE_OWNER) return L"take ownership";
            if (mask & WRITE_DAC) return L"change permissions";
            if (mask & DELETE) return L"delete";
            if (mask & FILE_DELETE_CHILD) return L"delete subfolders and files";
            if (mask & FILE_WRITE_DATA) return L"write data / create files";
            if (mask & FILE_APPEND_DATA) return L"append data / create folders";
            if (mask & FILE_WRITE_EA) return L"write extended attributes";
            if (mask & FILE_WRITE_ATTRIBUTES) return L"write attributes";
            return L"write";
         }

         // The allow entries a DACL holds, as (SID, mask after mapping, inherited),
         // counting only those that apply to the object itself.
         struct Allow_
         {
            PSID sid;
            DWORD mask;
            bool inherited;
         };

         bool AllowsOf_(PACL dacl, std::vector<Allow_> &allows)
         {
            allows.clear();
            ACL_SIZE_INFORMATION size;
            memset(&size, 0, sizeof(size));
            if (!GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation))
               return false;

            GENERIC_MAPPING mapping = { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
            for (DWORD index = 0; index < size.AceCount; index++)
            {
               LPVOID entry = nullptr;
               if (!GetAce(dacl, index, &entry) || entry == nullptr)
                  return false;

               const ACE_HEADER *header = (const ACE_HEADER *) entry;
               if ((header->AceFlags & INHERIT_ONLY_ACE) != 0)
                  continue;

               // An allow entry, plain or conditional (callback): a condition
               // could be true, so a conditional grant counts as a grant. The
               // object-type forms belong to directory objects, not files, and a
               // file's DACL does not carry them; one that did would be refused
               // below rather than skipped.
               PSID sid = nullptr;
               DWORD mask = 0;
               if (header->AceType == ACCESS_ALLOWED_ACE_TYPE)
               {
                  const ACCESS_ALLOWED_ACE *allow = (const ACCESS_ALLOWED_ACE *) entry;
                  sid = (PSID) &allow->SidStart;
                  mask = allow->Mask;
               }
               else if (header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE)
               {
                  const ACCESS_ALLOWED_CALLBACK_ACE *allow = (const ACCESS_ALLOWED_CALLBACK_ACE *) entry;
                  sid = (PSID) &allow->SidStart;
                  mask = allow->Mask;
               }
               else if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE || header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE)
               {
                  // Not a file's; the rule cannot say who it names without reading
                  // the object type, so it is taken as a grant of everything to
                  // nobody trusted.
                  Allow_ unknown = { nullptr, FILE_ALL_ACCESS, (header->AceFlags & INHERITED_ACE) != 0 };
                  allows.push_back(unknown);
                  continue;
               }
               else
                  continue;

               MapGenericMask(&mask, &mapping);
               Allow_ item = { sid, mask, (header->AceFlags & INHERITED_ACE) != 0 };
               allows.push_back(item);
            }
            return true;
         }

         std::wstring WithoutLongPrefix_(const std::wstring &path)
         {
            if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0)
               return L"\\\\" + path.substr(8);
            if (path.compare(0, 4, L"\\\\?\\") == 0)
               return path.substr(4);
            return path;
         }

         std::wstring Parent_(const std::wstring &path)
         {
            size_t slash = path.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
               return std::wstring();
            // "C:\" is its own root: the parent of "C:\x" is "C:\".
            if (slash == 2 && path.size() > 1 && path[1] == L':')
               return path.substr(0, 3);
            return path.substr(0, slash);
         }

         std::wstring Join_(const std::wstring &folder, const std::wstring &name)
         {
            if (!folder.empty() && (folder.back() == L'\\' || folder.back() == L'/'))
               return folder + name;
            return folder + L"\\" + name;
         }

         bool IsReparsePoint_(const std::wstring &path, bool &exists)
         {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            exists = attributes != INVALID_FILE_ATTRIBUTES;
            return exists && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
         }

         std::wstring Quoted_(const std::wstring &path)
         {
            // icacls takes "C:\" as the root; a trailing backslash inside the
            // quotes would escape the closing quote on a command line, so the
            // root is written without its quotes.
            if (path.size() == 3 && path[1] == L':' && path[2] == L'\\')
               return path;
            return L"\"" + path + L"\"";
         }

         // Whether the DACL of the object at path has an allow entry for sid, and
         // whether the first such is inherited.
         bool EntryFor_(const std::wstring &path, PSID sid, DWORD rights, bool &present, bool &inherited)
         {
            present = false;
            inherited = false;
            std::vector<unsigned char> buffer;
            std::wstring why;
            if (!ReadDescriptor(path, buffer, why))
               return false;
            BOOL daclPresent = FALSE, daclDefaulted = FALSE;
            PACL dacl = nullptr;
            if (!GetSecurityDescriptorDacl((PSECURITY_DESCRIPTOR) buffer.data(), &daclPresent, &dacl, &daclDefaulted) || !daclPresent || dacl == nullptr)
               return false;
            std::vector<Allow_> allows;
            if (!AllowsOf_(dacl, allows))
               return false;
            for (const Allow_ &allow : allows)
            {
               if (allow.sid == nullptr || !EqualSid(allow.sid, sid) || (allow.mask & rights) == 0)
                  continue;
               present = true;
               inherited = allow.inherited;
               return true;
            }
            return true;
         }

         // Where an inherited entry comes from: the nearest folder above path whose
         // DACL holds an entry for sid that is its own.
         std::wstring SourceOfInherited_(const std::wstring &path, PSID sid)
         {
            // Any right: the entry an inherited write comes from may be written
            // with inheritance flags that make it look different on the parent.
            const DWORD anyRight = 0xFFFFFFFF;
            for (std::wstring folder = Parent_(path); !folder.empty(); folder = Parent_(folder))
            {
               bool present = false, inherited = false;
               if (EntryFor_(folder, sid, anyRight, present, inherited) && present && !inherited)
                  return folder;
               if (Parent_(folder) == folder)
                  break;
            }
            return std::wstring();
         }
      }

      std::wstring CurrentUserSid()
      {
         std::wstring sidText;
         HANDLE token = nullptr;
         if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return sidText;
         DWORD needed = 0;
         GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
         std::vector<unsigned char> buffer(needed ? needed : 1);
         if (GetTokenInformation(token, TokenUser, buffer.data(), (DWORD) buffer.size(), &needed))
            sidText = TextOfSid(((TOKEN_USER *) buffer.data())->User.Sid);
         CloseHandle(token);
         return sidText;
      }

      bool IsWorkId(const std::wstring &text)
      {
         if (text.size() != 32)
            return false;
         for (wchar_t c : text)
         {
            if (!IsLowerHex_(c))
               return false;
         }
         return true;
      }

      bool IsReleaseVersion(const std::wstring &text)
      {
         // ^[0-9]+(\.[0-9]+){1,3}(-[A-Za-z0-9.]{1,20})?$, by hand: <regex> is more
         // code than the pattern, and this runs elevated on a command line.
         size_t i = 0;
         int parts = 0;
         for (;;)
         {
            const size_t start = i;
            while (i < text.size() && IsDigit_(text[i]) && i - start < 9)
               i++;
            if (i == start || (i < text.size() && IsDigit_(text[i])))
               return false;
            parts++;
            if (i < text.size() && text[i] == L'.' && parts < 4)
            {
               i++;
               continue;
            }
            break;
         }
         if (parts < 2)
            return false;
         if (i == text.size())
            return true;
         if (text[i] != L'-')
            return false;
         i++;
         const size_t suffix = text.size() - i;
         if (suffix < 1 || suffix > 20)
            return false;
         for (; i < text.size(); i++)
         {
            const wchar_t c = text[i];
            if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || c == L'.'))
               return false;
         }
         return true;
      }

      bool IsToken(const std::string &text)
      {
         if (text.size() != 64)
            return false;
         for (char c : text)
         {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
               return false;
         }
         return true;
      }

      bool IsDecimal(const std::wstring &text, unsigned long &value)
      {
         value = 0;
         if (text.empty() || text.size() > 10)
            return false;
         unsigned long long total = 0;
         for (wchar_t c : text)
         {
            if (!IsDigit_(c))
               return false;
            total = total * 10 + (unsigned long long) (c - L'0');
         }
         if (total > 0xFFFFFFFFull)
            return false;
         value = (unsigned long) total;
         return true;
      }

      std::wstring TextOfSid(PSID sid)
      {
         if (sid == nullptr || !IsValidSid(sid))
            return L"(no SID)";
         LPWSTR text = nullptr;
         if (!ConvertSidToStringSidW(sid, &text) || text == nullptr)
            return L"(unreadable SID)";
         std::wstring result(text);
         LocalFree(text);
         return result;
      }

      std::wstring NameOfSid(PSID sid)
      {
         if (sid == nullptr || !IsValidSid(sid))
            return L"(an entry of another kind)";
         wchar_t name[256];
         wchar_t domain[256];
         DWORD nameLength = 256, domainLength = 256;
         SID_NAME_USE use;
         if (LookupAccountSidW(nullptr, sid, name, &nameLength, domain, &domainLength, &use))
         {
            if (domainLength > 0 && domain[0] != L'\0')
               return std::wstring(domain) + L"\\" + name;
            return std::wstring(name);
         }
         return TextOfSid(sid);
      }

      bool IsTrustedSid(PSID sid)
      {
         if (sid == nullptr || !IsValidSid(sid))
            return false;
         if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid))
            return true;
         return _wcsicmp(TextOfSid(sid).c_str(), TrustedInstallerSid) == 0;
      }

      bool EvaluateDescriptor(PSECURITY_DESCRIPTOR descriptor, Place place, Finding &finding)
      {
         finding = Finding();

         PSID owner = nullptr;
         BOOL ownerDefaulted = FALSE;
         if (!GetSecurityDescriptorOwner(descriptor, &owner, &ownerDefaulted) || owner == nullptr)
         {
            finding.failed = true;
            finding.reason = L"its owner could not be read";
            return false;
         }
         if (!IsTrustedSid(owner))
         {
            finding.failed = true;
            finding.owner = true;
            finding.sid = TextOfSid(owner);
            finding.principal = NameOfSid(owner);
            finding.right = L"owner (may change the permissions)";
            return false;
         }

         BOOL daclPresent = FALSE, daclDefaulted = FALSE;
         PACL dacl = nullptr;
         if (!GetSecurityDescriptorDacl(descriptor, &daclPresent, &dacl, &daclDefaulted))
         {
            finding.failed = true;
            finding.reason = L"its permissions could not be read";
            return false;
         }
         if (!daclPresent || dacl == nullptr)
         {
            finding.failed = true;
            finding.sid = L"S-1-1-0";
            finding.principal = L"Everyone";
            finding.right = L"full control (the object has no permissions list at all)";
            return false;
         }

         std::vector<Allow_> allows;
         if (!AllowsOf_(dacl, allows))
         {
            finding.failed = true;
            finding.reason = L"its permissions could not be read";
            return false;
         }

         const DWORD counted = place == PlaceAboveApp ? ReplaceRights_ : WriteRights;
         for (const Allow_ &allow : allows)
         {
            const DWORD offending = allow.mask & counted;
            if (offending == 0 || IsTrustedSid(allow.sid))
               continue;

            finding.failed = true;
            finding.sid = allow.sid ? TextOfSid(allow.sid) : std::wstring(L"(an entry of another kind)");
            finding.principal = allow.sid ? NameOfSid(allow.sid) : finding.sid;
            finding.right = RightName_(offending);
            finding.inherited = allow.inherited;
            return false;
         }

         return true;
      }

      bool ReadDescriptor(const std::wstring &path, std::vector<unsigned char> &descriptor, std::wstring &why)
      {
         descriptor.clear();

         // Through a handle opened without following a link at the last element,
         // so that the descriptor read is the link's own when there is one - which
         // the callers refuse anyway - and never its target's.
         HANDLE handle = CreateFileW(path.c_str(), READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
         if (handle == INVALID_HANDLE_VALUE)
         {
            why = L"it could not be opened to read its permissions (Windows error " + std::to_wstring(GetLastError()) + L")";
            return false;
         }

         PSECURITY_DESCRIPTOR raw = nullptr;
         const DWORD result = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                              nullptr, nullptr, nullptr, nullptr, &raw);
         CloseHandle(handle);
         if (result != ERROR_SUCCESS || raw == nullptr)
         {
            why = L"its permissions could not be read (Windows error " + std::to_wstring(result) + L")";
            return false;
         }

         const DWORD length = GetSecurityDescriptorLength(raw);
         descriptor.assign((const unsigned char *) raw, (const unsigned char *) raw + length);
         LocalFree(raw);
         return true;
      }

      bool CheckObject(const std::wstring &path, Place place, const std::wstring &top, Finding &finding)
      {
         finding = Finding();
         std::vector<unsigned char> descriptor;
         std::wstring why;
         if (!ReadDescriptor(path, descriptor, why))
         {
            finding.failed = true;
            finding.object = path;
            finding.reason = why;
            return false;
         }

         if (EvaluateDescriptor((PSECURITY_DESCRIPTOR) descriptor.data(), place, finding))
            return true;

         finding.object = path;
         if (!finding.reason.empty())
            return false;

         if (finding.owner)
         {
            finding.fix = L"icacls " + Quoted_(path) + L" /setowner *S-1-5-32-544";
            return false;
         }

         // The SID again, as a SID, for the tracing below.
         PSID sid = nullptr;
         if (!ConvertStringSidToSidW(finding.sid.c_str(), &sid))
            sid = nullptr;

         if (!finding.inherited || sid == nullptr)
         {
            finding.fix = L"icacls " + Quoted_(path) + L" /remove:g *" + finding.sid;
         }
         else
         {
            finding.inheritedFrom = SourceOfInherited_(path, sid);

            // The highest folder at or below top that still has the entry by
            // inheritance: stopping it inheriting there, and removing the entry,
            // mends everything below it at once - without touching the folder the
            // entry is set on, which may be a drive's root that other programs
            // rely on.
            std::wstring target = path;
            if (place == PlaceInside && !top.empty())
            {
               for (std::wstring folder = Parent_(path); !folder.empty() && folder.size() >= top.size(); folder = Parent_(folder))
               {
                  bool present = false, inherited = false;
                  if (!EntryFor_(folder, sid, WriteRights, present, inherited) || !present || !inherited)
                     break;
                  target = folder;
                  if (SamePath(folder, top))
                     break;
               }
            }

            if (place == PlaceInside)
               finding.fix = L"icacls " + Quoted_(target) + L" /inheritance:d && icacls " + Quoted_(target) + L" /remove:g *" + finding.sid;
            else if (!finding.inheritedFrom.empty())
               finding.fix = L"icacls " + Quoted_(finding.inheritedFrom) + L" /remove:g *" + finding.sid;
            else
               finding.fix = L"icacls " + Quoted_(path) + L" /inheritance:d && icacls " + Quoted_(path) + L" /remove:g *" + finding.sid;
         }

         if (sid != nullptr)
            LocalFree(sid);
         return false;
      }

      namespace
      {
         bool CheckAncestors_(const std::wstring &path, Finding &finding)
         {
            // From the volume's root down to path's parent.
            std::vector<std::wstring> chain;
            for (std::wstring folder = Parent_(path); !folder.empty(); folder = Parent_(folder))
            {
               chain.push_back(folder);
               if (Parent_(folder) == folder)
                  break;
            }
            std::reverse(chain.begin(), chain.end());

            for (const std::wstring &folder : chain)
            {
               bool exists = false;
               if (IsReparsePoint_(folder, exists) && folder.size() > 3)
               {
                  finding = Finding();
                  finding.failed = true;
                  finding.object = folder;
                  finding.reason = L"it is a link or a junction, so what is below it is not where its path says";
                  return false;
               }
               if (!CheckObject(folder, PlaceAboveApp, std::wstring(), finding))
                  return false;
            }
            return true;
         }

         bool CheckFolderTree_(const std::wstring &folder, const std::wstring &app, bool recurse, Finding &finding)
         {
            bool exists = false;
            if (IsReparsePoint_(folder, exists))
            {
               finding = Finding();
               finding.failed = true;
               finding.object = folder;
               finding.reason = L"it is a link or a junction, so what is in it is not where its path says";
               return false;
            }
            if (!exists)
               return true;
            if (!CheckObject(folder, PlaceInside, app, finding))
               return false;
            if (!recurse)
               return true;

            WIN32_FIND_DATAW data;
            HANDLE search = FindFirstFileExW(Join_(folder, L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
            if (search == INVALID_HANDLE_VALUE)
               return true;

            std::vector<std::wstring> folders;
            bool ok = true;
            do
            {
               const std::wstring name = data.cFileName;
               if (name == L"." || name == L"..")
                  continue;
               const std::wstring child = Join_(folder, name);
               if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
               {
                  finding = Finding();
                  finding.failed = true;
                  finding.object = child;
                  finding.reason = L"it is a link, so what runs from it is not where its path says";
                  ok = false;
                  break;
               }
               if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
               {
                  folders.push_back(child);
                  continue;
               }
               std::wstring lower = name;
               std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return (wchar_t) towlower(c); });
               const bool code = lower.size() > 4 && (lower.compare(lower.size() - 4, 4, L".exe") == 0 || lower.compare(lower.size() - 4, 4, L".dll") == 0);
               if (!code)
                  continue;
               if (!CheckObject(child, PlaceInside, app, finding))
               {
                  ok = false;
                  break;
               }
            } while (FindNextFileW(search, &data));
            FindClose(search);

            if (!ok)
               return false;
            for (const std::wstring &child : folders)
            {
               if (!CheckFolderTree_(child, app, true, finding))
                  return false;
            }
            return true;
         }
      }

      bool CheckInstallation(const std::wstring &appPath, const std::vector<std::wstring> &extraFiles, Finding &finding)
      {
         finding = Finding();
         std::wstring app = WithoutLongPrefix_(appPath);
         while (app.size() > 3 && (app.back() == L'\\' || app.back() == L'/'))
            app.pop_back();

         if (!CheckAncestors_(app, finding))
            return false;

         // {app} itself: its own entries, not its files (the message store is
         // below it, in Data, and is the service's).
         if (!CheckFolderTree_(app, app, false, finding))
            return false;

         // A missing Bin is not "nothing to check": the stage runs from it.
         bool binExists = false;
         IsReparsePoint_(Join_(app, L"Bin"), binExists);
         if (!binExists)
         {
            finding = Finding();
            finding.failed = true;
            finding.object = Join_(app, L"Bin");
            finding.reason = L"it is not there";
            return false;
         }

         if (!CheckFolderTree_(Join_(app, L"Bin"), app, true, finding))
            return false;

         const std::wstring ini = Join_(Join_(app, L"Bin"), L"hMailServer.INI");
         if (GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES && !CheckObject(ini, PlaceInside, app, finding))
            return false;

         if (!CheckFolderTree_(Join_(app, L"ControlPanel"), app, true, finding))
            return false;
         if (!CheckFolderTree_(Join_(app, L"AdminUpdate"), app, false, finding))
            return false;

         for (const std::wstring &file : extraFiles)
         {
            if (file.empty())
               continue;
            if (!CheckAncestors_(file, finding))
               return false;
            // The file itself a link: its own permissions say nothing of the
            // file the stage would read through it.
            bool exists = false;
            if (IsReparsePoint_(file, exists))
            {
               finding = Finding();
               finding.failed = true;
               finding.object = file;
               finding.reason = L"it is a link, so what is read through it is not where its path says";
               return false;
            }
            if (!CheckObject(file, PlaceInside, std::wstring(), finding))
               return false;
         }

         return true;
      }

      bool MakeDescriptor(bool folder, const std::vector<std::wstring> &readers, unsigned long readerRights, const std::wstring &ownerSid,
                          std::vector<unsigned char> &descriptor, std::wstring &why, bool permissionsReadable)
      {
         // SDDL: owner, then a protected DACL (P) of SYSTEM and Administrators with
         // full control, handed down (OICI) on a folder, and each reader with the
         // rights asked for on this object alone.
         std::wstring sddl = L"O:" + (ownerSid.empty() ? std::wstring(L"BA") : ownerSid) + L"D:P";
         const wchar_t *inherit = folder ? L"OICI" : L"";
         sddl += std::wstring(L"(A;") + inherit + L";FA;;;SY)";
         sddl += std::wstring(L"(A;") + inherit + L";FA;;;BA)";
         // Reading the permissions (READ_CONTROL) and nothing else, on this
         // object alone: what an unelevated Control Panel needs to check them.
         if (permissionsReadable)
            sddl += L"(A;;RC;;;AU)";
         for (const std::wstring &reader : readers)
         {
            if (reader.empty())
               continue;
            wchar_t rights[16];
            swprintf_s(rights, L"0x%08lx", readerRights);
            sddl += L"(A;;" + std::wstring(rights) + L";;;" + reader + L")";
         }

         PSECURITY_DESCRIPTOR made = nullptr;
         ULONG size = 0;
         if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &made, &size) || made == nullptr)
         {
            why = L"its permissions could not be made (Windows error " + std::to_wstring(GetLastError()) + L")";
            return false;
         }
         descriptor.assign((const unsigned char *) made, (const unsigned char *) made + size);
         LocalFree(made);
         return true;
      }

      bool FinalPathOf(HANDLE handle, std::wstring &path)
      {
         std::vector<wchar_t> buffer(32768);
         const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), (DWORD) buffer.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
         if (length == 0 || length >= buffer.size())
            return false;
         path = WithoutLongPrefix_(std::wstring(buffer.data(), length));
         return true;
      }

      bool SamePath(const std::wstring &left, const std::wstring &right)
      {
         std::wstring a = WithoutLongPrefix_(left);
         std::wstring b = WithoutLongPrefix_(right);
         while (a.size() > 3 && (a.back() == L'\\' || a.back() == L'/'))
            a.pop_back();
         while (b.size() > 3 && (b.back() == L'\\' || b.back() == L'/'))
            b.pop_back();
         std::replace(a.begin(), a.end(), L'/', L'\\');
         std::replace(b.begin(), b.end(), L'/', L'\\');
         return CompareStringOrdinal(a.c_str(), (int) a.size(), b.c_str(), (int) b.size(), TRUE) == CSTR_EQUAL;
      }

      bool MakeOrOpenAdminFolder(const std::wstring &path, bool mustBeNew, const std::wstring &ownerSid, HANDLE &held, std::wstring &why,
                                 const std::vector<std::wstring> &readers, bool permissionsReadable)
      {
         held = INVALID_HANDLE_VALUE;

         std::vector<unsigned char> descriptor;
         if (!MakeDescriptor(true, readers, FILE_GENERIC_READ, ownerSid, descriptor, why, permissionsReadable))
            return false;

         SECURITY_ATTRIBUTES attributes;
         memset(&attributes, 0, sizeof(attributes));
         attributes.nLength = sizeof(attributes);
         attributes.lpSecurityDescriptor = descriptor.data();
         attributes.bInheritHandle = FALSE;

         bool made = CreateDirectoryW(path.c_str(), &attributes) != FALSE;
         if (!made)
         {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS || mustBeNew)
            {
               why = error == ERROR_ALREADY_EXISTS
                  ? path + L" is already there, and this folder must be new"
                  : path + L" could not be made (Windows error " + std::to_wstring(error) + L")";
               return false;
            }
         }

         // Not shared for delete: while this handle is open the folder cannot be
         // removed or renamed, and neither can any folder above it. Windows
         // enforces a handle's sharing only when it was opened for some access to
         // the data - FILE_LIST_DIRECTORY here - and a handle with attributes and
         // READ_CONTROL alone holds nothing: the tester renamed one.
         held = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
         if (held == INVALID_HANDLE_VALUE)
         {
            why = path + L" could not be held open (Windows error " + std::to_wstring(GetLastError()) + L")";
            return false;
         }

         FILE_ATTRIBUTE_TAG_INFO tag;
         memset(&tag, 0, sizeof(tag));
         std::wstring final;
         bool ok = GetFileInformationByHandleEx(held, FileAttributeTagInfo, &tag, sizeof(tag)) != FALSE;
         if (!ok)
            why = path + L" could not be examined (Windows error " + std::to_wstring(GetLastError()) + L")";
         else if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
         {
            ok = false;
            why = path + L" is a link or a junction";
         }
         else if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
         {
            ok = false;
            why = path + L" is not a folder";
         }
         else if (!FinalPathOf(held, final) || !SamePath(final, path))
         {
            ok = false;
            why = path + L" resolves to " + (final.empty() ? std::wstring(L"somewhere Windows cannot name") : final) + L", not to itself";
         }

         if (ok && !made)
         {
            // One that was there before: it must be what this would have made - an
            // owner and permissions nobody but administrators could have set.
            Finding finding;
            if (!CheckObject(path, PlaceInside, std::wstring(), finding))
            {
               ok = false;
               why = path + L" was already there with permissions nobody but administrators should have: " +
                     (finding.reason.empty() ? finding.principal + L" may " + finding.right : finding.reason);
            }
         }

         if (!ok)
         {
            CloseHandle(held);
            held = INVALID_HANDLE_VALUE;
         }
         return ok;
      }

      HANDLE CreateAdminFile(const std::wstring &path, const std::vector<std::wstring> &readers, unsigned long readerRights,
                             const std::wstring &ownerSid, std::wstring &why)
      {
         std::vector<unsigned char> descriptor;
         if (!MakeDescriptor(false, readers, readerRights, ownerSid, descriptor, why))
            return INVALID_HANDLE_VALUE;

         SECURITY_ATTRIBUTES attributes;
         memset(&attributes, 0, sizeof(attributes));
         attributes.nLength = sizeof(attributes);
         attributes.lpSecurityDescriptor = descriptor.data();
         attributes.bInheritHandle = FALSE;

         HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, &attributes, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
         if (file == INVALID_HANDLE_VALUE)
            why = path + L" could not be made (Windows error " + std::to_wstring(GetLastError()) + L")";
         return file;
      }

      HANDLE HoldForReading(const std::wstring &path, std::wstring &why)
      {
         HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
         if (file == INVALID_HANDLE_VALUE)
         {
            why = path + L" could not be held open (Windows error " + std::to_wstring(GetLastError()) + L")";
            return INVALID_HANDLE_VALUE;
         }
         FILE_ATTRIBUTE_TAG_INFO tag;
         memset(&tag, 0, sizeof(tag));
         if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) ||
             (tag.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
         {
            why = path + L" is a link, a folder, or could not be examined";
            CloseHandle(file);
            return INVALID_HANDLE_VALUE;
         }
         return file;
      }

      bool SafeCopy(const std::wstring &source, const std::wstring &expectedFinalPath, unsigned long long cap, HANDLE destination,
                    unsigned long long &copied, std::wstring &why)
      {
         copied = 0;
         HANDLE input = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
         if (input == INVALID_HANDLE_VALUE)
         {
            const DWORD error = GetLastError();
            why = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
               ? source + L" is not there"
               : source + L" could not be opened (Windows error " + std::to_wstring(error) + L")";
            return false;
         }

         bool ok = true;
         FILE_ATTRIBUTE_TAG_INFO tag;
         memset(&tag, 0, sizeof(tag));
         FILE_STANDARD_INFO standard;
         memset(&standard, 0, sizeof(standard));
         std::wstring final;
         if (!GetFileInformationByHandleEx(input, FileAttributeTagInfo, &tag, sizeof(tag)) ||
             !GetFileInformationByHandleEx(input, FileStandardInfo, &standard, sizeof(standard)))
         {
            ok = false;
            why = source + L" could not be examined (Windows error " + std::to_wstring(GetLastError()) + L")";
         }
         else if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
         {
            ok = false;
            why = source + L" is a link, not a file";
         }
         else if (standard.Directory)
         {
            ok = false;
            why = source + L" is a folder, not a file";
         }
         else if ((unsigned long long) standard.EndOfFile.QuadPart > cap)
         {
            ok = false;
            why = source + L" is " + std::to_wstring((unsigned long long) standard.EndOfFile.QuadPart) + L" bytes, more than the " +
                  std::to_wstring(cap) + L" an installer may be";
         }
         else if (!FinalPathOf(input, final) || !SamePath(final, expectedFinalPath))
         {
            ok = false;
            why = source + L" resolves to " + (final.empty() ? std::wstring(L"somewhere Windows cannot name") : final) +
                  L", not to " + expectedFinalPath + L": a link or a junction on the way leads elsewhere";
         }

         if (ok)
         {
            std::vector<unsigned char> buffer(1024 * 1024);
            for (;;)
            {
               DWORD got = 0;
               if (!ReadFile(input, buffer.data(), (DWORD) buffer.size(), &got, nullptr))
               {
                  ok = false;
                  why = source + L" could not be read (Windows error " + std::to_wstring(GetLastError()) + L")";
                  break;
               }
               if (got == 0)
                  break;
               if (copied + got > cap)
               {
                  ok = false;
                  why = source + L" grew past " + std::to_wstring(cap) + L" bytes while it was read";
                  break;
               }
               DWORD written = 0;
               if (!WriteFile(destination, buffer.data(), got, &written, nullptr) || written != got)
               {
                  ok = false;
                  why = L"the copy of " + source + L" could not be written (Windows error " + std::to_wstring(GetLastError()) + L")";
                  break;
               }
               copied += got;
            }
         }

         CloseHandle(input);
         if (ok && !FlushFileBuffers(destination))
         {
            ok = false;
            why = L"the copy of " + source + L" could not be flushed (Windows error " + std::to_wstring(GetLastError()) + L")";
         }
         return ok;
      }

      std::string Sha256Of(HANDLE file, std::wstring &why)
      {
         std::string hex;
         BCRYPT_ALG_HANDLE algorithm = nullptr;
         if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
         {
            why = L"SHA-256 is not available";
            return hex;
         }
         BCRYPT_HASH_HANDLE hash = nullptr;
         if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0)))
         {
            BCryptCloseAlgorithmProvider(algorithm, 0);
            why = L"SHA-256 could not be started";
            return hex;
         }

         LARGE_INTEGER start;
         start.QuadPart = 0;
         bool ok = SetFilePointerEx(file, start, nullptr, FILE_BEGIN) != FALSE;
         std::vector<unsigned char> buffer(1024 * 1024);
         while (ok)
         {
            DWORD got = 0;
            if (!ReadFile(file, buffer.data(), (DWORD) buffer.size(), &got, nullptr))
               ok = false;
            else if (got == 0)
               break;
            else if (!BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), got, 0)))
               ok = false;
         }

         unsigned char digest[32];
         if (ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, (ULONG) sizeof(digest), 0)))
         {
            const char *digits = "0123456789abcdef";
            for (size_t i = 0; i < sizeof(digest); i++)
            {
               hex += digits[digest[i] >> 4];
               hex += digits[digest[i] & 0x0F];
            }
         }
         else
            why = L"the file could not be read to its end for its SHA-256";

         BCryptDestroyHash(hash);
         BCryptCloseAlgorithmProvider(algorithm, 0);
         return hex;
      }

      Architecture ArchitectureFromDescription(const std::wstring &description)
      {
         const size_t open = description.rfind(L'(');
         const size_t close = description.rfind(L')');
         if (open == std::wstring::npos || close == std::wstring::npos || close < open)
            return ArchitectureX64;

         std::wstring marker = description.substr(open + 1, close - open - 1);
         std::transform(marker.begin(), marker.end(), marker.begin(), [](wchar_t c) { return (wchar_t) towlower(c); });
         if (marker == L"x64")
            return ArchitectureX64;
         if (marker == L"arm64")
            return ArchitectureArm64;
         return ArchitectureOther;
      }

      Architecture BuildArchitecture()
      {
#if defined(_M_ARM64)
         return ArchitectureArm64;
#else
         return ArchitectureX64;
#endif
      }

      const wchar_t *ArchitectureName(Architecture architecture)
      {
         switch (architecture)
         {
         case ArchitectureX64: return L"x64";
         case ArchitectureArm64: return L"ARM64";
         default: return L"another architecture";
         }
      }

      bool ReadVersionResource(const std::wstring &path, std::wstring &fixedVersion, std::wstring &description, std::wstring &why)
      {
         fixedVersion.clear();
         description.clear();
         DWORD ignored = 0;
         const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
         if (size == 0)
         {
            why = L"it carries no version resource";
            return false;
         }
         std::vector<unsigned char> data(size);
         VS_FIXEDFILEINFO *info = nullptr;
         UINT length = 0;
         if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()) ||
             !VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID *>(&info), &length) ||
             info == nullptr || length < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xFEEF04BD)
         {
            why = L"its version resource cannot be read";
            return false;
         }
         wchar_t text[64];
         swprintf_s(text, L"%u.%u.%u.%u", (unsigned) HIWORD(info->dwFileVersionMS), (unsigned) LOWORD(info->dwFileVersionMS),
                    (unsigned) HIWORD(info->dwFileVersionLS), (unsigned) LOWORD(info->dwFileVersionLS));
         fixedVersion = text;

         struct Translation
         {
            WORD language;
            WORD codePage;
         } *translations = nullptr;
         UINT translationBytes = 0;
         if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID *>(&translations), &translationBytes) &&
             translations != nullptr && translationBytes >= sizeof(Translation))
         {
            wchar_t query[64];
            swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[0].language, translations[0].codePage);
            wchar_t *value = nullptr;
            UINT valueLength = 0;
            if (VerQueryValueW(data.data(), query, reinterpret_cast<LPVOID *>(&value), &valueLength) && value != nullptr && valueLength > 0)
               description.assign(value, wcsnlen(value, valueLength));
         }
         while (!description.empty() && (description.back() == L' ' || description.back() == L'\0'))
            description.pop_back();
         return true;
      }

      int CompareVersions(const std::wstring &left, const std::wstring &right)
      {
         auto parse = [](const std::wstring &text, unsigned long parts[4])
         {
            for (int i = 0; i < 4; i++)
               parts[i] = 0;
            size_t i = 0;
            if (i < text.size() && (text[i] == L'v' || text[i] == L'V'))
               i++;
            int part = 0;
            for (; i < text.size() && part < 4; i++)
            {
               if (IsDigit_(text[i]))
               {
                  if (parts[part] < 100000000)
                     parts[part] = parts[part] * 10 + (unsigned long) (text[i] - L'0');
               }
               else if (text[i] == L'.')
                  part++;
               else
                  break;
            }
         };
         unsigned long a[4], b[4];
         parse(left, a);
         parse(right, b);
         for (int i = 0; i < 4; i++)
         {
            if (a[i] != b[i])
               return a[i] < b[i] ? -1 : 1;
         }
         return 0;
      }
   }
}

#endif
