// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A child process with the caller's rights taken away, for parsing input the
// caller must not parse at its own rights.
//
// hMailServer.exe /InstallUpdate runs as an elevated administrator, and the one
// input it has to parse that the service composed byte by byte is the Sigstore
// bundle. So it hands the bundle to hMailServer.exe --verify-bundle started
// through here (AdminUpdate.h):
//
//   - the caller's own token restricted: every privilege removed but
//     SeChangeNotifyPrivilege (DISABLE_MAX_PRIVILEGE), the Administrators group
//     and every other group UAC filters made deny-only (IsFilteredByUac, and
//     whatever the elevated token's filtered half holds only to deny), and the
//     integrity lowered to Low (S-1-16-4096) - less than the approving user's
//     ordinary, unelevated rights: at Medium a child a crafted bundle took over
//     could still write that user's profile and registry, which the user's own
//     programs and Windows' auto-elevating ones read, and at Low it can write
//     nothing of the user's;
//   - started suspended and put in a job before it runs anything: one process,
//     a memory limit, killed when the job's handle closes and when it faults,
//     and no reach into the desktop, the clipboard or other processes' windows;
//   - given two pipes and nothing else (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): its
//     standard input carries the request, its standard output the answer, its
//     standard error goes to NUL;
//   - run from System32, and killed when it has not answered in time.
//
// The PST reader's process (Pst/PstReaderProcess.cpp) is launched the same way,
// from the service; it keeps its own launch code, which is unchanged by this.
//
// Win32 and the C++ library only, and nothing written anywhere: it runs in the
// elevated stage before anything of the server's is set up.
#ifndef HM_PLATFORM_POSIX

#include <functional>
#include <string>

namespace HM
{
   namespace LoweredChild
   {
      struct Request
      {
         Request() : timeoutMilliseconds(120000), memoryLimitBytes(512ull * 1024 * 1024), maximumOutputBytes(64 * 1024) {}

         std::wstring program;             // the full path of the program
         std::wstring arguments;           // appended to the quoted program path
         std::string input;                // written to the child's standard input, which is then closed
         unsigned long timeoutMilliseconds;
         unsigned long long memoryLimitBytes;
         size_t maximumOutputBytes;        // what is read of the standard output; the rest is refused

         // Called with the child's process handle while it is still suspended, in
         // its job, before it has run an instruction: the tester reads its token
         // back here. Optional.
         std::function<void(HANDLE process)> inspect;
      };

      struct Result
      {
         Result() : started(false), exitCode(0), timedOut(false), outputTruncated(false) {}

         bool started;
         unsigned long exitCode;
         bool timedOut;
         bool outputTruncated;
         std::string output;
         std::wstring error;               // why it could not be started, in one sentence
      };

      // Runs the request and waits for the child, for at most its timeout (then
      // it is killed, with the job). False when it could not be started as
      // described - a token that could not be lowered, a job that could not be
      // made - in which case nothing ran. True otherwise, whatever the child did;
      // the caller judges exitCode and output.
      bool Run(const Request &request, Result &result);

      // The restricted primary token Run starts the child with: the caller's own,
      // with DISABLE_MAX_PRIVILEGE, every group UAC filters deny-only and Low
      // integrity. NULL with error.
      HANDLE MakeToken(unsigned long &error);

      // Whether UAC makes this group deny-only in an administrator's filtered
      // token: the Administrators, Power Users, the Account, Server, Print,
      // Backup, Network Configuration and Cryptographic Operators, RAS Servers,
      // Pre-Windows 2000 Compatible Access, "Local account and member of
      // Administrators group", and a domain's Domain Admins, Enterprise Admins,
      // Schema Admins, Cert Publishers, Group Policy Creator Owners and domain
      // controllers.
      bool IsFilteredByUac(PSID sid);

      // What a token says about itself, for the tester: whether the
      // Administrators group is in it only to deny, whether it is enabled, which
      // privileges are enabled, which groups UAC filters are enabled, and its
      // integrity level's RID.
      struct TokenFacts
      {
         TokenFacts() : administratorsPresent(false), administratorsDenyOnly(false), administratorsEnabled(false), integrityRid(0) {}

         bool administratorsPresent;
         bool administratorsDenyOnly;
         bool administratorsEnabled;
         std::wstring enabledPrivileges;      // names, comma-separated
         std::wstring enabledFilteredGroups;  // SIDs, comma-separated: none in a lowered child's token
         unsigned long integrityRid;
      };
      bool ReadTokenFacts(HANDLE token, TokenFacts &facts);
   }
}

#endif
