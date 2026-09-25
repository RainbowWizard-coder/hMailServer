// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstImportPath.h.

#include "StdAfx.h"

#include "PstImportPath.h"
#include "Unicode.h"

#ifdef HM_PLATFORM_POSIX
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#endif

#include <cwctype>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      bool IsSeparator(wchar_t c)
      {
         return c == L'/' || c == L'\\';
      }
   }

   bool
   PstImportPath::FinalPathOf(const String &path, String &finalPath, bool &isDirectory)
   {
      finalPath.Empty();
      isDirectory = false;

#ifdef HM_PLATFORM_POSIX
      AnsiString utf8;
      Unicode::WideToMultiByte(path, utf8);

      char resolved[PATH_MAX + 1];
      if (::realpath(utf8.c_str(), resolved) == nullptr)
         return false;

      struct stat status;
      if (::stat(resolved, &status) != 0)
         return false;

      isDirectory = S_ISDIR(status.st_mode);
      if (!isDirectory && !S_ISREG(status.st_mode))
         return false;

      Unicode::MultiByteToWide(AnsiString(resolved), finalPath);
      return true;
#else
      // Opened for its attributes only, and shared every way, so that nothing
      // holding the file - Outlook - is disturbed by the look; a directory needs
      // FILE_FLAG_BACKUP_SEMANTICS to be opened at all.
      HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
      if (handle == INVALID_HANDLE_VALUE)
         return false;

      BY_HANDLE_FILE_INFORMATION information;
      bool known = GetFileInformationByHandle(handle, &information) != 0;
      if (known)
         isDirectory = (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

      std::wstring resolved;
      const DWORD needed = known ? GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) : 0;
      if (needed > 0 && needed < 32768)
      {
         std::vector<wchar_t> buffer(needed + 1);
         const DWORD written = GetFinalPathNameByHandleW(handle, &buffer[0], needed + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
         if (written > 0 && written <= needed)
            resolved.assign(&buffer[0], written);
      }

      CloseHandle(handle);

      if (resolved.empty())
         return false;

      finalPath = Comparable(resolved);
      return !finalPath.IsEmpty();
#endif
   }

   String
   PstImportPath::Comparable(const std::wstring &finalPath)
   {
#ifdef HM_PLATFORM_POSIX
      return String(finalPath);
#else
      // \\?\C:\... is a local path; \\?\UNC\server\share\... is not.
      String path(finalPath);
      if (path.Left(8).CompareNoCase(_T("\\\\?\\UNC\\")) == 0)
         return String();
      if (path.Left(4) == _T("\\\\?\\"))
         path = path.Mid(4);
      if (path.Left(2) == _T("\\\\") || path.Left(2) == _T("//"))
         return String();
      return path;
#endif
   }

   bool
   PstImportPath::IsInside(const String &path, const String &directory)
   {
      String parent = directory;
      while (parent.GetLength() > 1 && IsSeparator(parent[parent.GetLength() - 1]))
         parent = parent.Left(parent.GetLength() - 1);

      if (parent.IsEmpty() || path.GetLength() <= parent.GetLength() + 1)
         return false;

      const String head = path.Left(parent.GetLength());
      if (!IsSeparator(path[parent.GetLength()]))
         return false;

#ifdef HM_PLATFORM_POSIX
      return head == parent;
#else
      return head.CompareNoCase(parent) == 0;
#endif
   }

   String
   PstImportPath::RelativeName(const String &file, const String &directory)
   {
      String parent = directory;
      while (parent.GetLength() > 1 && IsSeparator(parent[parent.GetLength() - 1]))
         parent = parent.Left(parent.GetLength() - 1);

      if (!IsInside(file, parent))
         return String();

      String relative = file.Mid(parent.GetLength() + 1);
      relative.Replace(_T("\\"), _T("/"));
      return relative;
   }

   bool
   PstImportPath::IsPlainName(const String &name, String &problem)
   {
      const String refusal = _T("Name the file as it is in the domain's PST import directory - a name such as archive.pst, or sub/archive.pst - not a path, a drive or a share.");

      if (name.IsEmpty() || name.GetLength() > 255)
      {
         problem = refusal;
         return false;
      }

      if (IsSeparator(name[0]) || name.Find(_T(":")) >= 0)
      {
         problem = refusal;
         return false;
      }

      String component;
      for (int i = 0; i <= name.GetLength(); i++)
      {
         const wchar_t c = i < name.GetLength() ? name[i] : L'/';
         if (c < 0x20 || c == 0x7F || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|')
         {
            problem = refusal;
            return false;
         }

         if (IsSeparator(c))
         {
            if (component.IsEmpty() || component == _T(".") || component == _T(".."))
            {
               problem = refusal;
               return false;
            }
            component.Empty();
            continue;
         }

         component += c;
      }

      return true;
   }

   String
   PstImportPath::SettingProblem(const String &directory)
   {
      String value = directory;
      value.Trim();
      if (value.IsEmpty())
         return String();

      const String refusal = _T("pst_import_directory must be empty, or an absolute path to a directory on this machine: not a relative path, and not a share on another one.");

#ifdef HM_PLATFORM_POSIX
      if (value[0] != L'/')
         return refusal;
#else
      if (value.GetLength() < 3 || !iswalpha(value[0]) || value[1] != L':' || !IsSeparator(value[2]))
         return refusal;
#endif

      return String();
   }

   bool
   PstImportPath::Resolve(const String &name, const String &domain, String &file, String &directory, String &problem, bool &missing)
   {
      missing = false;
      file.Empty();
      directory.Empty();

      String root = Configuration::Instance()->GetPstImportDirectory();
      root.Trim();
      if (root.IsEmpty())
      {
         problem = _T("No PST import directory is set on this server (pst_import_directory); set one, copy the file into its subdirectory named for the account's domain, and name it there.");
         return false;
      }

      if (!SettingProblem(root).IsEmpty())
      {
         problem = SettingProblem(root);
         return false;
      }

      if (!IsPlainName(name, problem))
         return false;

      String lowerDomain = domain;
      lowerDomain.MakeLower();
      while (root.GetLength() > 1 && IsSeparator(root[root.GetLength() - 1]))
         root = root.Left(root.GetLength() - 1);
      const String domainDirectory = root + FileUtilities::PathSeparator + lowerDomain;

      bool isDirectory = false;
      if (!FinalPathOf(domainDirectory, directory, isDirectory) || !isDirectory)
      {
         directory.Empty();
         problem = Formatter::Format(_T("There is no directory {0} for this domain in the PST import directory; create it and copy the file there."), lowerDomain);
         return false;
      }

      String relative = name;
#ifndef HM_PLATFORM_POSIX
      relative.Replace(_T("/"), _T("\\"));
#endif

      const String candidate = directory + FileUtilities::PathSeparator + relative;
      if (!FinalPathOf(candidate, file, isDirectory))
      {
         file.Empty();
         missing = true;
         problem = Formatter::Format(_T("There is no file {0} in this domain's directory ({1}) of the PST import directory; copy it there, or check its name."), name, lowerDomain);
         return false;
      }

      if (isDirectory)
      {
         file.Empty();
         problem = Formatter::Format(_T("{0} is a directory, not a PST file."), name);
         return false;
      }

      // Followed to where it really is: a link or a junction inside the
      // domain's directory that leads out of it is refused, as a name with ..
      // in it is.
      if (!IsInside(file, directory))
      {
         file.Empty();
         problem = Formatter::Format(_T("The name {0} leads outside this domain's PST import directory (through a link or a junction), which is refused."), name);
         return false;
      }

      return true;
   }
}
