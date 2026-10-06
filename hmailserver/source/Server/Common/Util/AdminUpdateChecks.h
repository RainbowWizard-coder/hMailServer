// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The checks an administrator's install of an update makes on the files and
// folders it is about to trust (docs/ServiceAccount.md, "The live update").
//
// A service running as NT SERVICE\hMailServer cannot run an installer, so the
// Control Panel on the server installs the update itself, after Windows' own
// elevation prompt: hMailServer.exe /InstallUpdate (AdminUpdate.h) runs
// elevated, copies the installer the service downloaded into a folder only
// administrators can write, holds it there, and checks it; hMailServer.Updater
// then runs it. Everything here is one of the checks that stage makes, written
// against Win32 and the C++ library alone - no String, no logger, no settings -
// so that it runs before anything of the server's is set up, writes nothing
// anywhere, and can be tested on its own.
//
//   - The permission rule: nobody but SYSTEM, the Administrators and
//     TrustedInstaller may change the program that is run elevated, the folder
//     it is in, or a folder on the way to it. The Control Panel applies the same
//     rule before it asks for elevation (Services/InstallationPermissions.cs),
//     since a replaced hMailServer.exe would replace its own check; both are
//     held to one table of cases (build/testdata/install-permissions-cases.json).
//   - The work folder, {app}\AdminUpdate\<id>: made new, owned by the
//     Administrators, with a protected DACL of SYSTEM and Administrators alone,
//     opened without following a link and held so that it cannot be renamed.
//   - The safe copy out of the service's Updates folder: the source opened
//     without following a link, refused when it is one or when Windows resolves
//     it anywhere but where it was asked for, copied through that handle (the
//     main stream only) into a file made new in the work folder.
//   - The small ones: the work id, the version, the token, the architecture the
//     installer's version resource names.
#ifndef HM_PLATFORM_POSIX

#include <string>
#include <vector>

namespace HM
{
   namespace AdminUpdateChecks
   {
      // TrustedInstaller's service SID, the owner of Windows' own files.
      extern const wchar_t *const TrustedInstallerSid;

      // The file rights that change what an object holds, what it is called or
      // who may change it, once the generic rights are mapped: what the rule
      // counts on {app} and below (PlaceInside), and the one definition of a
      // right to write that hMailServer.exe /GrantServiceAccess and
      // /ProtectFolders check the program's folders against (ServiceAccount.cpp).
      extern const unsigned long WriteRights;

      // 32 lowercase hexadecimal characters: the work folder's name, which the
      // Control Panel makes and the stage takes from its command line.
      bool IsWorkId(const std::wstring &text);

      // A release as the feed names one and the stage accepts it on its command
      // line: ^[0-9]+(\.[0-9]+){1,3}(-[A-Za-z0-9.]{1,20})?$
      bool IsReleaseVersion(const std::wstring &text);

      // 64 lowercase hexadecimal characters: UpdateApplyToken's form.
      bool IsToken(const std::string &text);

      // Decimal digits, at most ten, whose value fits a DWORD.
      bool IsDecimal(const std::wstring &text, unsigned long &value);

      // ------------------------------------------------------------------ the permission rule

      // Where an object stands relative to the installation, {app}:
      //   PlaceAboveApp  a folder on the way to {app}. Only what would let the next
      //                  element be replaced counts: FILE_DELETE_CHILD, DELETE,
      //                  WRITE_DAC or WRITE_OWNER for anybody untrusted, or an owner
      //                  who is not trusted. Authenticated Users' "create folders"
      //                  on C:\ passes.
      //   PlaceInside    {app} and everything checked below it, and a trust file:
      //                  every right that writes, deletes or changes permissions
      //                  counts, and the owner must be trusted.
      enum Place
      {
         PlaceAboveApp,
         PlaceInside
      };

      // What failed, for the text an administrator reads and the command that
      // removes it.
      struct Finding
      {
         Finding() : failed(false), owner(false), inherited(false) {}

         bool failed;
         std::wstring object;          // the path
         std::wstring principal;       // DOMAIN\name, or the SID where there is no name
         std::wstring sid;             // S-1-...
         std::wstring right;           // the first offending right, in words
         bool owner;                   // the principal is the object's owner
         bool inherited;               // the entry is inherited
         std::wstring inheritedFrom;   // the folder the entry is set on, when found
         std::wstring fix;             // the icacls command(s) that remove it
         std::wstring reason;          // when the permissions could not be read at all
      };

      // True for SYSTEM, the Administrators and TrustedInstaller: the principals a
      // file run elevated may be changeable by.
      bool IsTrustedSid(PSID sid);

      // The rule on one descriptor, with no file access: the owner, then every
      // allow entry that applies to the object itself (inherit-only entries do
      // not, and deny entries are ignored, which can only make the rule stricter).
      // CREATOR OWNER counts as untrusted in an entry that applies; in an
      // inherit-only one it is a template and is skipped with the rest. A NULL
      // DACL fails. On failure, finding says who may do what; the path, the fix
      // and inheritedFrom are CheckObject's to fill in.
      bool EvaluateDescriptor(PSECURITY_DESCRIPTOR descriptor, Place place, Finding &finding);

      // The descriptor of the object at path (owner and DACL), read without
      // following a link at the last element: false with why when it cannot be read.
      bool ReadDescriptor(const std::wstring &path, std::vector<unsigned char> &descriptor, std::wstring &why);

      // One object: read, evaluated, and on failure the icacls line(s) that remove
      // what failed. top is the highest folder the fix may be made on ({app} for
      // PlaceInside objects); an inherited entry is traced up from the object to
      // the folder it is set on.
      bool CheckObject(const std::wstring &path, Place place, const std::wstring &top, Finding &finding);

      // The whole rule for an installation, in this order, stopping at the first
      // failure:
      //   - every folder from the volume's root down to {app}'s parent (PlaceAboveApp),
      //     each also refused when it is a link or a junction;
      //   - {app}, {app}\Bin, {app}\ControlPanel and {app}\AdminUpdate when there
      //     is one, and every folder below Bin and ControlPanel, each refused when
      //     it is a link;
      //   - every *.exe and *.dll in Bin and ControlPanel, at any depth, and
      //     Bin\hMailServer.INI; Bin\hMailServerApiKeys.ini is not code, nothing
      //     elevated reads it, and it is the one file there the service may write;
      //   - each extra file (the Sigstore trust files hMailServer.INI names), with
      //     the folders on its way as PlaceAboveApp, and refused when it is itself
      //     a link.
      // app is the folder above Bin, without a trailing separator. From 6.3.7
      // hMailServer.exe /ProtectFolders gives {app}, Bin and ControlPanel the
      // permissions this passes (ServiceAccount::Harden), then holds the
      // installation to this and says its first finding as CHECK.
      bool CheckInstallation(const std::wstring &app, const std::vector<std::wstring> &extraFiles, Finding &finding);

      // The principal's name as Windows gives it, DOMAIN\name; the SID's text when
      // it has none.
      std::wstring NameOfSid(PSID sid);
      std::wstring TextOfSid(PSID sid);

      // The SID of the account this process runs as.
      std::wstring CurrentUserSid();

      // ------------------------------------------------------------------ folders and files

      // A security descriptor for a folder or file only administrators may
      // change: owner Administrators, a protected DACL of SYSTEM and
      // Administrators with full control (inherited by what is made below, for a
      // folder), and each of readers with readerRights on the object itself only.
      // ownerSid empty means Administrators; the testers pass the caller's own SID
      // when they do not run elevated, since only an administrator's token may
      // make the Administrators an owner. With permissionsReadable, Authenticated
      // Users may also read the object's permissions (READ_CONTROL, on the object
      // itself only) and nothing else of it: see MakeOrOpenAdminFolder.
      bool MakeDescriptor(bool folder, const std::vector<std::wstring> &readers, unsigned long readerRights, const std::wstring &ownerSid,
                          std::vector<unsigned char> &descriptor, std::wstring &why, bool permissionsReadable = false);

      // Makes the folder with MakeDescriptor's permissions and opens it without
      // following a link, shared for reading and writing but not for delete, so
      // that it cannot be renamed or removed while held. With mustBeNew, a folder
      // already there is refused (ERROR_ALREADY_EXISTS); without it, one already
      // there is opened, and refused when it is a link, when its owner is not
      // trusted or when its DACL fails the rule. Either way the held handle's final
      // path must be the path asked for. readers are given read on the folder
      // itself (MakeDescriptor): the stage gives none - a reader opens a file in
      // it by name, through bypass traverse checking - and an unelevated tester
      // gives itself read so that it can hold what it made.
      //
      // permissionsReadable is for {app}\AdminUpdate alone, which the Control
      // Panel holds to the permission rule before it asks Windows for the
      // elevation (Services/InstallationPermissions.cs) and so reads unelevated:
      // made with Authenticated Users able to read its permissions, it can be
      // checked by the Control Panel of every administrator, and the second
      // install on a server is not refused because the first made the folder.
      // Its contents are made by the stage with their own permissions and are
      // the administrators' as before.
      bool MakeOrOpenAdminFolder(const std::wstring &path, bool mustBeNew, const std::wstring &ownerSid, HANDLE &held, std::wstring &why,
                                 const std::vector<std::wstring> &readers = std::vector<std::wstring>(), bool permissionsReadable = false);

      // A file made new (CREATE_NEW, a link at its name is not followed) with
      // MakeDescriptor's permissions, opened for writing and shared for reading.
      HANDLE CreateAdminFile(const std::wstring &path, const std::vector<std::wstring> &readers, unsigned long readerRights,
                             const std::wstring &ownerSid, std::wstring &why);

      // The path Windows resolved an open handle to, with \\?\ taken off.
      bool FinalPathOf(HANDLE handle, std::wstring &path);

      // Two paths the same, compared as Windows compares names: case-insensitively,
      // after taking off \\?\ and a trailing separator.
      bool SamePath(const std::wstring &left, const std::wstring &right);

      // Copies source into destination, a file this process made new and holds
      // open for writing. The source is opened for reading, shared for reading
      // only, without following a link, and refused when it is a reparse point,
      // a directory, larger than cap, or when the path Windows resolves its handle
      // to is not expectedFinalPath (a junction or a link anywhere on the way).
      // The main stream only: CopyFile would carry the alternate streams too,
      // Zone.Identifier among them. copied is the number of bytes.
      bool SafeCopy(const std::wstring &source, const std::wstring &expectedFinalPath, unsigned long long cap, HANDLE destination,
                    unsigned long long &copied, std::wstring &why);

      // Opens a file for reading, shared for reading only, without following a
      // link at its name: nothing can write, rename or delete it while the handle
      // is open, nor rename a folder above it. INVALID_HANDLE_VALUE with why.
      HANDLE HoldForReading(const std::wstring &path, std::wstring &why);

      // The SHA-256 of what a handle holds, from its first byte, as lowercase hex
      // (Windows' own CNG). Empty with why when it cannot be read.
      std::string Sha256Of(HANDLE file, std::wstring &why);

      // ------------------------------------------------------------------ the installer's resource

      enum Architecture
      {
         ArchitectureX64,
         ArchitectureArm64,
         ArchitectureOther
      };

      // The architecture the installer's FileDescription names: from 6.3.6 on the
      // installers say "hMailServer Setup (x64)" or "hMailServer Setup (ARM64)"
      // (section_setup_64.iss, section_setup_arm64.iss). A description without a
      // parenthesised marker is x64: every Windows installer before 6.3.6 was x64
      // and said "hMailServer Setup". Any other marker is ArchitectureOther.
      Architecture ArchitectureFromDescription(const std::wstring &description);

      // This build's own: ARM64 under _M_ARM64, x64 otherwise.
      Architecture BuildArchitecture();
      const wchar_t *ArchitectureName(Architecture architecture);

      // The fixed file version ("6.3.7.0") and the first translation's
      // FileDescription of a file's version resource.
      bool ReadVersionResource(const std::wstring &path, std::wstring &fixedVersion, std::wstring &description, std::wstring &why);

      // Numeric, component by component, as UpdateChecker::CompareVersions: a
      // leading "v" and anything from the first character that is neither a digit
      // nor a dot are ignored; missing components are zero. <0, 0 or >0.
      int CompareVersions(const std::wstring &left, const std::wstring &right);
   }
}

#endif
