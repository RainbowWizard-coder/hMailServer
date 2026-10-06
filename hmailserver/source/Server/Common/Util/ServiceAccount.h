// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The account the server runs as, and - on Windows - what the service's own
// identity is granted so that it can run as something less than LocalSystem
// (docs/ServiceAccount.md).
//
// A new Windows installation registers the service as the per-service virtual
// account NT SERVICE\hMailServer: no password, no account to create, a SID that
// Windows derives from the service's name (S-1-5-80-...). What that account may
// touch is exactly what it has been granted, and the grant is
// hMailServer.exe /GrantServiceAccess, run elevated by the installer and by the
// Control Panel's button.
//
// WHAT THE GRANT TRUSTS, which is the whole of its safety. It runs elevated, so
// a folder it grants is a folder an administrator's token wrote a permission
// on - and the service can write its database, and everything it has Modify
// on. So nothing the service can write decides where a grant goes:
//
//   - the program folder is where this binary is ({app}\Bin\hMailServer.exe),
//     not [Directories] ProgramFolder;
//   - Modify, handed down, goes to that folder's own Data, Logs, Temp and
//     Database folders, and to the folders an administrator approved in
//     HKEY_LOCAL_MACHINE\SOFTWARE\hMailServer\ServiceAccessPaths, a key only
//     administrators may write - the installer and the Control Panel add to it
//     after a confirmation that names each folder, and the grant refuses the
//     list if anybody else may write the key;
//   - Read goes to each program folder in the program folder - Bin, Events and
//     the rest - but not to the program folder itself, which bypass traverse
//     checking lets the account pass through, and whose own entry would make
//     Windows walk the message store below it;
//   - a folder hMailServer.INI or the database names that is none of those is
//     REPORTED, never granted; and no folder is granted, approved or not, that
//     is or holds the program folder, is in it but outside those four, is or
//     is in or holds the Windows folder, a Program Files folder or the user
//     profiles, is or holds ProgramData, is a drive's root, is not on a fixed
//     local drive, or is or runs through a junction or link that the
//     administrators do not alone control (OnlyAdministratorsControl,
//     ProtectedFile.h) - one they alone control is gone through, as the store
//     commands go through it, and where it leads is held to every refusal here;
//   - a certificate or key file is given Read, on the file alone, only when it
//     lies in one of those folders; one elsewhere is listed, and the move to
//     the virtual account is not made silently while there is one;
//   - hMailServer.INI is given NOTHING. It holds [Database], [Directories] and
//     [Security], which elevated commands read, and the service runs on a
//     read-only copy (IniFileSettings::InitializationFileWritable).
//
// Each grant adds one entry for the service's SID and removes nothing, so an
// administrator's own entries are never touched, and a second run finds the
// entries there and changes nothing. While a folder is being granted, it and
// every folder above it are held open without sharing delete, so none of them
// can be renamed or swapped for a link underneath the grant; a file is granted
// through a handle opened without following links, and only when it has one
// name (no hard link elsewhere).
//
// The grant names the service SID, NT SERVICE\hMailServer, rather than the logon
// account: the service's token carries that SID whatever account it logs on as,
// because hMailServer.exe /Register sets the service's SID type to unrestricted
// for any account but LocalSystem (ServiceManager), so a group managed service
// account reaches the same folders through the same entries.
//
// What stays LocalSystem's alone - installing an update, writing a firewall
// rule - is asked of ProcessIsAdministrator() by the code that needs it, and
// refused with a sentence rather than attempted and failed.
namespace HM
{
   class ServiceAccount
   {
   public:
      // The account this process runs as, as the system names it:
      // "NT AUTHORITY\SYSTEM", "NT SERVICE\hMailServer", "DOMAIN\user", or on
      // POSIX the user name. Empty when it cannot be read.
      static String ProcessAccountName();

      // True when this process runs as the most privileged account on the
      // machine: LocalSystem on Windows, root on POSIX.
      static bool ProcessIsMostPrivileged();

      // True when this process may administer the machine - run an installer,
      // write a firewall rule: on Windows a token with BUILTIN\Administrators
      // enabled, which LocalSystem's has and a virtual account's has not; on
      // POSIX root.
      static bool ProcessIsAdministrator();

#ifndef HM_PLATFORM_POSIX
      // The service SID of a service name, "S-1-5-80-" and five numbers, as
      // Windows derives it: the SHA-1 of the name in upper case, UTF-16LE, read
      // as five little-endian 32-bit numbers. It needs no service of that name
      // to exist, which is what lets the tester grant a SID of its own.
      static String ServiceSid(const String &serviceName);

      // The masks the grant uses: Modify (read, write, execute and delete -
      // icacls M), Read and execute (icacls RX), for program folders, and Read
      // (icacls R), for a certificate or key file.
      static const DWORD RightsModify = 0x001301BF;
      static const DWORD RightsReadExecute = 0x001200A9;
      static const DWORD RightsRead = 0x00120089;

      enum GrantResult
      {
         GrantMade = 0,
         GrantAlreadyThere = 1,
         GrantFailed = 2
      };

      // Grants sid the rights on path: on a directory, one allow entry
      // inherited by every folder and file under it (and Windows then walks the
      // tree to hand it down, which on a large message store takes minutes); on
      // a file, one allow entry of its own. Nothing is removed and no other
      // entry is changed. GrantAlreadyThere, and nothing written, when an allow
      // entry for sid covers the rights (and, on a directory, reaches both
      // folders and files below it) - its own, or an inherited one no inherited
      // deny entry for sid stands against. detail says why a grant failed, and
      // names a deny entry the grant cannot outrank or one it now comes before.
      // Takes the path as given: RunGrantCommand decides which paths may be
      // granted and holds them while it does.
      static GrantResult Grant(const String &path, const String &sid, DWORD rights, bool directory, String &detail);

      // Whether an allow entry for sid covers the rights on path, as Grant
      // judges it. False, with error, when the permissions cannot be read.
      static bool HasGrant(const String &path, const String &sid, DWORD rights, bool directory, String &error);

      // HKEY_LOCAL_MACHINE\ this: the folders an administrator has approved for
      // the service, one value each - the folder's full path as the name,
      // "Modify" or "Read" as the data (REG_SZ). In the 64-bit view.
      static String ApprovedPathsKey() { return _T("SOFTWARE\\hMailServer\\ServiceAccessPaths"); }

      // Where certificate and key files are kept so that the service reaches
      // them: Data\Certificates in the program folder, inside the data folder's
      // own grant. The secure-defaults row (new installations) makes it; at the
      // fold this is NewInstallDefaults::CertificateDirectory(). Empty when this
      // binary is not in an installation's Bin folder.
      static String CertificateDirectory();

      // What /GrantServiceAccess is asked to do, from its command line
      // (hMailServer.cpp).
      struct GrantOptions
      {
         // --confirm: when the configuration names folders no administrator has
         // approved, or key files outside every granted folder, ask - in a box
         // on the desktop of this elevated process, which nothing running at a
         // lower integrity can answer - before approving and granting them. No
         // changes nothing at all. The installer passes it for an attended
         // installation, the Control Panel always.
         bool confirm;

         // --report-only: say what would be granted, approved and refused, and
         // change nothing.
         bool reportOnly;

         // --approve-modify <folder> and --approve-read <folder>: folders to
         // approve before granting - checked against the same refusals, then
         // recorded in ApprovedPathsKey().
         std::vector<std::pair<String, DWORD> > approve;

         GrantOptions() : confirm(false), reportOnly(false) {}
      };

      // hMailServer.exe /GrantServiceAccess. One line per grant - made, already
      // there, or failed with the reason - and per path the configuration names
      // that is not granted: NOT APPROVED, OUTSIDE or REFUSED. Exit codes:
      //   0  every grant made, and everything the configuration names reached
      //      (or the administrator confirmed what is not);
      //   1  a grant or an approval failed;
      //   2  this binary is not in the Bin folder of an installation;
      //   3  not run as an administrator;
      //   4  the configuration names folders not approved, or key files outside
      //      the grants, and nobody confirmed them (no --confirm, or
      //      --report-only);
      //   5  the configuration names a folder that is never granted - the
      //      service would not reach it - so the configuration needs changing;
      //   6  the administrator answered No; nothing was changed;
      //   7  the grants were made, but the database could not be read by this
      //      account, so what it names - certificate and key files, the backup
      //      and PST import folders - is not known, and nobody confirmed
      //      (--confirm) a move without knowing it. Until 6.3.6 this was 0.
      // The installer and the Control Panel move the service only on 0.
      static int RunGrantCommand(const GrantOptions &options);

      // Makes what is at path private to the machine's administrators, SYSTEM
      // and the service: its permissions no longer inherited, so that nothing
      // the folder above hands down reaches it - the Read Program Files hands
      // BUILTIN\Users, the rights a folder at a drive's root hands every user;
      // every deny entry and every entry of its own kept, an inherited one as one
      // of its own when it is the service's SID's; SYSTEM and the Administrators
      // given full control, on a directory handed down to everything below; and,
      // with serviceReads, Read for sid when nothing gives it that. Every other
      // inherited allow entry is dropped, and its account named in dropped.
      // GrantAlreadyThere, and nothing written, when the permissions inherit
      // nothing already (and sid may read, with serviceReads); GrantFailed with
      // detail when they could not be read or written. Takes the path as given:
      // RunProtectCommand decides which paths, and holds them while it does.
      static GrantResult Protect(const String &path, const String &sid, bool directory, bool serviceReads, String &detail, std::vector<String> &dropped);

      // Makes what is at path - one of the program's own folders, which every
      // user may read and run and nobody but SYSTEM and the administrators may
      // change - what Program Files leaves one: its permissions no longer
      // inherited, so that nothing the folder above hands down reaches it - the
      // Modify a drive's root hands Authenticated Users on a Windows client, the
      // right to make files and folders it hands Users on Windows Server; every
      // deny entry and every entry of its own kept, the grant's Read for sid
      // among them; an inherited entry for sid kept as one of its own while it
      // gives no right to write; SYSTEM and the Administrators given full
      // control and BUILTIN\Users Read & execute, on a directory handed down to
      // everything below; and its owner made the Administrators when it is not
      // SYSTEM, the Administrators or TrustedInstaller, naming the one it had in
      // formerOwner - an owner may always change the permissions. Every other
      // inherited allow entry is dropped, and named in dropped when it gave a
      // right to write (AdminUpdateChecks::WriteRights). GrantAlreadyThere, and
      // nothing written, when the permissions and the owner are that already;
      // GrantFailed with detail when they could not be read or written. A file
      // or folder below with permissions of its own keeps them. Takes the path
      // as given: RunProtectCommand decides which paths, and holds them while it
      // does.
      static GrantResult Harden(const String &path, const String &sid, bool directory, String &detail, std::vector<String> &dropped, String &formerOwner);

      // What /ProtectFolders is asked to do, from its command line.
      struct ProtectOptions
      {
         // --report-only: say what would be made private, and change nothing.
         bool reportOnly;

         ProtectOptions() : reportOnly(false) {}
      };

      // hMailServer.exe /ProtectFolders, run elevated by the installer at every
      // installation and upgrade of the server, whatever account the service runs
      // as, and by hand. A default installation's Data, Logs, Temp, Database and
      // Events folders and Bin\hMailServer.INI inherited Read for BUILTIN\Users
      // from Program Files, so every user of the computer could read the mail,
      // the logs, the built-in database and the database's password; this makes
      // each Protect()ed - the program folder's own, found from where this binary
      // is, a junction or mount point among them gone through only when the
      // administrators alone control it, as the grant goes through one. What the
      // configuration names elsewhere, and a key file in the data folder with
      // permissions of its own, is CHECKed for a broad group's Read and never
      // changed. Then the program folder itself, Bin and ControlPanel are each
      // Harden()ed - SYSTEM and the Administrators full control, BUILTIN\Users
      // Read & execute, nothing inherited, the owner the Administrators - so
      // that no user of the computer may change a program the service or an
      // administrator runs, as an installation at a drive's root let every
      // user until 6.3.7; the program folder only once none of the five folders
      // above failed, since its change is handed down through them. And the
      // installation is held to the rule of an administrator's install of an
      // update (AdminUpdateChecks::CheckInstallation), which what this hardens
      // passes: what it leaves - an entry of a file's own, a folder above the
      // program folder - is CHECKed, with the icacls that removes it. One line
      // each - made private, already private, hardened, already hardened,
      // FAILED or CHECK. Exit codes: 0 every one made private or hardened, or
      // already so (CHECK lines are said, and do not change it); 1 one could
      // not be; 2 this binary is not in the Bin folder of an installation; 3
      // not run as an administrator.
      static int RunProtectCommand(const ProtectOptions &options);

      // hMailServer.exe --grant-database-login: what the service's own identity,
      // NT SERVICE\hMailServer, needs to open its database when that is SQL Server
      // reached with Windows authentication on this computer - a login FROM
      // WINDOWS, a user for it in the database hMailServer.INI names, and that
      // user in db_owner - made over a connection of this process's own, as the
      // account that runs it (docs/ServiceAccount.md, "SQL Server with Windows
      // authentication"). The installer runs it, elevated, before it moves the
      // service to the virtual account, so that the moved service opens the
      // database the database step made as LocalSystem; DBSetup runs it when it
      // finishes a new installation; an administrator runs it by hand. What is
      // there already is left as it is, and nothing is taken away; nothing is
      // made in a database with DDL triggers of its own, which would run with
      // the rights of the account that runs this. A SQL Server
      // on another computer is left alone: there the virtual account signs in as
      // this computer's own account, as LocalSystem does. One line per step, and
      // for a step it could not take, the statements a SQL Server administrator
      // runs instead. Exit codes:
      //   0  the service's identity can open the database - its login, its user
      //      and db_owner, made now or there already - or needs nothing here:
      //      the database is not SQL Server reached with Windows authentication,
      //      or that SQL Server runs on another computer (the lines say which), so
      //      that "--grant-database-login && ... /Register" moves only a service
      //      that will open its database;
      //   1  one of them could not be made;
      //   2  hMailServer.INI names no database, or the database could not be
      //      opened as this account.
      static int RunDatabaseLoginCommand();
#endif
   };

#ifndef HM_PLATFORM_POSIX
   // The grant on a directory of the tester's own under the temp folder: made,
   // made again (nothing changes), inherited by a file already in it, the
   // administrator's own entry kept, a file with protected permissions granted
   // without losing its protection, a hard-linked file and a path through a
   // link the service's account could change refused, one through a junction
   // the administrators alone control held to where it leads; which folders a
   // grant may reach, a malicious
   // [Directories] value among them; the logon /Register passes for each
   // --service-account; what /ProtectFolders makes of a folder and a file
   // that inherit BUILTIN\Users' Read (TestProtect_); and the program's own
   // folders it hardens - the permissions it builds from what a drive's root,
   // Windows Server's and Program Files hand down, held to the rule of an
   // administrator's install of an update (TestHardenedShape_), and an
   // installation's folders on the disk that inherit Authenticated Users'
   // Modify (TestHarden_). See the definition.
   class ServiceAccountTester
   {
   public:
      void Test();
   };
#endif
}
