// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#ifndef HM_PLATFORM_POSIX

#include "LoweredChild.h"

#include <sddl.h>

#include <algorithm>
#include <thread>
#include <vector>

// Nothing in this file writes a log line, reports an error or reads a setting:
// it runs inside hMailServer.exe /InstallUpdate, elevated, before anything of the
// server's own is set up (build/check-elevated-verbs.py holds it to that).

namespace HM
{
   namespace LoweredChild
   {
      namespace
      {
         std::wstring ErrorText_(const wchar_t *what, DWORD error)
         {
            return std::wstring(what) + L" (Windows error " + std::to_wstring(error) + L")";
         }

         std::wstring System32_()
         {
            wchar_t buffer[MAX_PATH];
            const UINT length = GetSystemDirectoryW(buffer, MAX_PATH);
            return length > 0 && length < MAX_PATH ? std::wstring(buffer, length) : std::wstring(L"C:\\Windows\\System32");
         }

         std::wstring SidText_(PSID sid)
         {
            LPWSTR text = nullptr;
            if (sid == nullptr || !ConvertSidToStringSidW(sid, &text) || text == nullptr)
               return std::wstring();
            std::wstring result(text);
            LocalFree(text);
            return result;
         }

         // A token's groups, as TokenGroups gives them.
         bool GroupsOf_(HANDLE token, std::vector<unsigned char> &buffer)
         {
            DWORD needed = 0;
            GetTokenInformation(token, TokenGroups, nullptr, 0, &needed);
            buffer.assign(needed ? needed : 1, 0);
            return GetTokenInformation(token, TokenGroups, buffer.data(), (DWORD) buffer.size(), &needed) != FALSE;
         }

         // The SIDs the administrator's own filtered token - the other half of an
         // elevated token, what the approving user's unelevated programs run with
         // - holds only to deny. Empty for a token that is not the elevated half
         // of a pair (an unelevated caller, a service, UAC off).
         std::vector<std::wstring> DenyOnlyInFilteredHalf_(HANDLE own)
         {
            std::vector<std::wstring> sids;
            TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
            DWORD size = 0;
            if (!GetTokenInformation(own, TokenElevationType, &type, sizeof(type), &size) || type != TokenElevationTypeFull)
               return sids;
            TOKEN_LINKED_TOKEN linked;
            memset(&linked, 0, sizeof(linked));
            if (!GetTokenInformation(own, TokenLinkedToken, &linked, sizeof(linked), &size) || linked.LinkedToken == nullptr)
               return sids;
            std::vector<unsigned char> buffer;
            if (GroupsOf_(linked.LinkedToken, buffer))
            {
               const TOKEN_GROUPS *list = (const TOKEN_GROUPS *) buffer.data();
               for (DWORD i = 0; i < list->GroupCount; i++)
               {
                  if ((list->Groups[i].Attributes & SE_GROUP_USE_FOR_DENY_ONLY) != 0)
                     sids.push_back(SidText_(list->Groups[i].Sid));
               }
            }
            CloseHandle(linked.LinkedToken);
            return sids;
         }
      }

      bool IsFilteredByUac(PSID sid)
      {
         const std::wstring text = SidText_(sid);
         if (text.empty())
            return false;
         // Administrators, Power Users, Account, Server, Print and Backup
         // Operators, RAS Servers, Pre-Windows 2000 Compatible Access, Network
         // Configuration Operators, Cryptographic Operators, and "Local account
         // and member of Administrators group".
         static const wchar_t *const exact[] = { L"S-1-5-32-544", L"S-1-5-32-547", L"S-1-5-32-548", L"S-1-5-32-549", L"S-1-5-32-550", L"S-1-5-32-551",
                                                 L"S-1-5-32-553", L"S-1-5-32-554", L"S-1-5-32-556", L"S-1-5-32-569", L"S-1-5-114" };
         for (const wchar_t *candidate : exact)
         {
            if (_wcsicmp(text.c_str(), candidate) == 0)
               return true;
         }
         // A domain's Enterprise Read-only Domain Controllers, Domain Admins,
         // Domain Controllers, Cert Publishers, Schema Admins, Enterprise Admins,
         // Group Policy Creator Owners and Read-only Domain Controllers.
         if (text.compare(0, 9, L"S-1-5-21-") == 0)
         {
            const std::wstring rid = text.substr(text.rfind(L'-') + 1);
            static const wchar_t *const rids[] = { L"498", L"512", L"516", L"517", L"518", L"519", L"520", L"521" };
            for (const wchar_t *candidate : rids)
            {
               if (rid == candidate)
                  return true;
            }
         }
         return false;
      }

      HANDLE MakeToken(unsigned long &error)
      {
         error = 0;
         HANDLE own = nullptr;
         if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, &own))
         {
            error = GetLastError();
            return nullptr;
         }

         // Every group the approving user's unelevated token holds only to deny,
         // made deny-only here too: the Administrators, and with them every other
         // group UAC takes away - Backup Operators, Domain Admins and the rest
         // (IsFilteredByUac), and whatever the filtered half of this very token
         // says besides. A SID in SidsToDisable that the token does not hold, or
         // holds deny-only already (an unelevated caller's), is ignored.
         std::vector<unsigned char> groups;
         if (!GroupsOf_(own, groups))
         {
            error = GetLastError();
            CloseHandle(own);
            return nullptr;
         }
         const std::vector<std::wstring> filteredHalf = DenyOnlyInFilteredHalf_(own);
         std::vector<SID_AND_ATTRIBUTES> disable;
         {
            const TOKEN_GROUPS *list = (const TOKEN_GROUPS *) groups.data();
            for (DWORD i = 0; i < list->GroupCount; i++)
            {
               const DWORD attributes = list->Groups[i].Attributes;
               if ((attributes & (SE_GROUP_USE_FOR_DENY_ONLY | SE_GROUP_INTEGRITY | SE_GROUP_LOGON_ID)) != 0)
                  continue;
               const std::wstring text = SidText_(list->Groups[i].Sid);
               bool deny = IsFilteredByUac(list->Groups[i].Sid);
               for (const std::wstring &sid : filteredHalf)
                  deny = deny || _wcsicmp(sid.c_str(), text.c_str()) == 0;
               if (!deny)
                  continue;
               SID_AND_ATTRIBUTES entry;
               entry.Sid = list->Groups[i].Sid;
               entry.Attributes = 0;
               disable.push_back(entry);
            }
         }

         HANDLE restricted = nullptr;
         const BOOL made = CreateRestrictedToken(own, DISABLE_MAX_PRIVILEGE, (DWORD) disable.size(), disable.empty() ? nullptr : disable.data(), 0, nullptr,
                                                 0, nullptr, &restricted);
         if (!made)
            error = GetLastError();
         CloseHandle(own);
         if (!made)
            return nullptr;

         // Low integrity. An elevated caller's token is High; Medium, the approving
         // user's own level, would still let a child that a crafted bundle took
         // over write that user's profile and registry - where the user's own
         // programs, and Windows' auto-elevating ones, read what they run - which
         // is a step to the administrator's rights. At Low it can read what it
         // needs (its program, Bin and System32) and write nothing of the user's.
         // Lowering is allowed without any privilege; raising is not.
         BYTE lowBuffer[SECURITY_MAX_SID_SIZE];
         DWORD lowSize = sizeof(lowBuffer);
         if (!CreateWellKnownSid(WinLowLabelSid, nullptr, lowBuffer, &lowSize))
         {
            error = GetLastError();
            CloseHandle(restricted);
            return nullptr;
         }

         // Only when the token is above Low: a token already at Low or below keeps
         // its level (a service's is System, which this lowers too).
         TokenFacts facts;
         if (!ReadTokenFacts(restricted, facts))
         {
            error = GetLastError();
            CloseHandle(restricted);
            return nullptr;
         }
         if (facts.integrityRid > SECURITY_MANDATORY_LOW_RID)
         {
            TOKEN_MANDATORY_LABEL label;
            memset(&label, 0, sizeof(label));
            label.Label.Sid = (PSID) lowBuffer;
            label.Label.Attributes = SE_GROUP_INTEGRITY;
            if (!SetTokenInformation(restricted, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid((PSID) lowBuffer)))
            {
               error = GetLastError();
               CloseHandle(restricted);
               return nullptr;
            }
         }
         return restricted;
      }

      bool ReadTokenFacts(HANDLE token, TokenFacts &facts)
      {
         facts = TokenFacts();

         BYTE administratorsBuffer[SECURITY_MAX_SID_SIZE];
         DWORD administratorsSize = sizeof(administratorsBuffer);
         if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administratorsBuffer, &administratorsSize))
            return false;

         DWORD needed = 0;
         GetTokenInformation(token, TokenGroups, nullptr, 0, &needed);
         std::vector<unsigned char> groups(needed ? needed : 1);
         if (!GetTokenInformation(token, TokenGroups, groups.data(), (DWORD) groups.size(), &needed))
            return false;
         const TOKEN_GROUPS *list = (const TOKEN_GROUPS *) groups.data();
         for (DWORD i = 0; i < list->GroupCount; i++)
         {
            const DWORD attributes = list->Groups[i].Attributes;
            if ((attributes & SE_GROUP_ENABLED) != 0 && (attributes & SE_GROUP_USE_FOR_DENY_ONLY) == 0 && IsFilteredByUac(list->Groups[i].Sid))
               facts.enabledFilteredGroups += (facts.enabledFilteredGroups.empty() ? L"" : L",") + SidText_(list->Groups[i].Sid);
            if (!EqualSid(list->Groups[i].Sid, (PSID) administratorsBuffer))
               continue;
            facts.administratorsPresent = true;
            facts.administratorsDenyOnly = (attributes & SE_GROUP_USE_FOR_DENY_ONLY) != 0;
            facts.administratorsEnabled = (attributes & SE_GROUP_ENABLED) != 0;
         }

         needed = 0;
         GetTokenInformation(token, TokenPrivileges, nullptr, 0, &needed);
         std::vector<unsigned char> privileges(needed ? needed : 1);
         if (!GetTokenInformation(token, TokenPrivileges, privileges.data(), (DWORD) privileges.size(), &needed))
            return false;
         const TOKEN_PRIVILEGES *held = (const TOKEN_PRIVILEGES *) privileges.data();
         for (DWORD i = 0; i < held->PrivilegeCount; i++)
         {
            if ((held->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED) == 0)
               continue;
            wchar_t name[128];
            DWORD length = 128;
            LUID luid = held->Privileges[i].Luid;
            if (LookupPrivilegeNameW(nullptr, &luid, name, &length))
               facts.enabledPrivileges += (facts.enabledPrivileges.empty() ? L"" : L",") + std::wstring(name);
         }

         needed = 0;
         GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &needed);
         std::vector<unsigned char> integrity(needed ? needed : 1);
         if (!GetTokenInformation(token, TokenIntegrityLevel, integrity.data(), (DWORD) integrity.size(), &needed))
            return false;
         const TOKEN_MANDATORY_LABEL *label = (const TOKEN_MANDATORY_LABEL *) integrity.data();
         const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
         if (count > 0)
            facts.integrityRid = *GetSidSubAuthority(label->Label.Sid, count - 1);
         return true;
      }

      bool Run(const Request &request, Result &result)
      {
         result = Result();

         SECURITY_ATTRIBUTES inheritable;
         memset(&inheritable, 0, sizeof(inheritable));
         inheritable.nLength = sizeof(inheritable);
         inheritable.bInheritHandle = TRUE;

         HANDLE childInput = nullptr, parentInput = nullptr, parentOutput = nullptr, childOutput = nullptr;
         HANDLE childError = INVALID_HANDLE_VALUE;
         auto closeAll = [&]()
         {
            if (childInput) CloseHandle(childInput);
            if (parentInput) CloseHandle(parentInput);
            if (parentOutput) CloseHandle(parentOutput);
            if (childOutput) CloseHandle(childOutput);
            if (childError != INVALID_HANDLE_VALUE) CloseHandle(childError);
            childInput = parentInput = parentOutput = childOutput = nullptr;
            childError = INVALID_HANDLE_VALUE;
         };

         // The input pipe's buffer holds the whole request, so that it is written
         // whether or not the child reads it; the output pipe's holds the most
         // that is read of the answer.
         const DWORD inputBuffer = (DWORD) (request.input.size() + 4096);
         if (!CreatePipe(&childInput, &parentInput, &inheritable, inputBuffer) ||
             !CreatePipe(&parentOutput, &childOutput, &inheritable, (DWORD) (request.maximumOutputBytes + 4096)))
         {
            result.error = ErrorText_(L"its pipes could not be made", GetLastError());
            closeAll();
            return false;
         }
         SetHandleInformation(parentInput, HANDLE_FLAG_INHERIT, 0);
         SetHandleInformation(parentOutput, HANDLE_FLAG_INHERIT, 0);

         childError = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
         if (childError == INVALID_HANDLE_VALUE)
         {
            result.error = ErrorText_(L"the null device could not be opened for it", GetLastError());
            closeAll();
            return false;
         }

         SIZE_T attributeSize = 0;
         InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
         std::vector<unsigned char> attributeBuffer(attributeSize ? attributeSize : 1);
         LPPROC_THREAD_ATTRIBUTE_LIST attributes = (LPPROC_THREAD_ATTRIBUTE_LIST) attributeBuffer.data();
         HANDLE inherited[3] = { childInput, childOutput, childError };
         if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize))
         {
            result.error = ErrorText_(L"the list of handles it inherits could not be made", GetLastError());
            closeAll();
            return false;
         }
         if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
         {
            result.error = ErrorText_(L"the list of handles it inherits could not be made", GetLastError());
            DeleteProcThreadAttributeList(attributes);
            closeAll();
            return false;
         }

         unsigned long tokenError = 0;
         HANDLE token = MakeToken(tokenError);
         if (token == nullptr)
         {
            result.error = ErrorText_(L"its restricted token could not be made", tokenError);
            DeleteProcThreadAttributeList(attributes);
            closeAll();
            return false;
         }

         STARTUPINFOEXW startup;
         memset(&startup, 0, sizeof(startup));
         startup.StartupInfo.cb = sizeof(startup);
         startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
         startup.StartupInfo.wShowWindow = SW_HIDE;
         startup.StartupInfo.hStdInput = childInput;
         startup.StartupInfo.hStdOutput = childOutput;
         startup.StartupInfo.hStdError = childError;
         startup.lpAttributeList = attributes;

         std::wstring commandLine = L"\"" + request.program + L"\"";
         if (!request.arguments.empty())
            commandLine += L" " + request.arguments;
         std::vector<wchar_t> line(commandLine.begin(), commandLine.end());
         line.push_back(L'\0');

         const std::wstring directory = System32_();
         PROCESS_INFORMATION information;
         memset(&information, 0, sizeof(information));
         const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
         const BOOL started = CreateProcessAsUserW(token, request.program.c_str(), line.data(), nullptr, nullptr, TRUE, flags, nullptr,
                                                   directory.c_str(), &startup.StartupInfo, &information);
         const DWORD startError = GetLastError();
         CloseHandle(token);
         DeleteProcThreadAttributeList(attributes);

         // The child has its copies, or there is no child.
         CloseHandle(childInput);
         CloseHandle(childOutput);
         CloseHandle(childError);
         childInput = childOutput = nullptr;
         childError = INVALID_HANDLE_VALUE;

         if (!started)
         {
            result.error = ErrorText_(L"it could not be started with its restricted token", startError);
            closeAll();
            return false;
         }

         HANDLE job = CreateJobObjectW(nullptr, nullptr);
         bool jobbed = job != nullptr;
         DWORD jobError = jobbed ? 0 : GetLastError();
         if (jobbed)
         {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
            memset(&limits, 0, sizeof(limits));
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
                                                      JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY |
                                                      JOB_OBJECT_LIMIT_PRIORITY_CLASS;
            limits.BasicLimitInformation.ActiveProcessLimit = 1;
            limits.BasicLimitInformation.PriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
            limits.ProcessMemoryLimit = (SIZE_T) request.memoryLimitBytes;
            jobbed = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE;

            if (jobbed)
            {
               JOBOBJECT_BASIC_UI_RESTRICTIONS interfaceLimits;
               memset(&interfaceLimits, 0, sizeof(interfaceLimits));
               interfaceLimits.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_EXITWINDOWS |
                                                     JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                                                     JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
               jobbed = SetInformationJobObject(job, JobObjectBasicUIRestrictions, &interfaceLimits, sizeof(interfaceLimits)) != FALSE;
            }
            jobbed = jobbed && AssignProcessToJobObject(job, information.hProcess) != FALSE;
            if (!jobbed)
               jobError = GetLastError();
         }

         if (!jobbed)
         {
            TerminateProcess(information.hProcess, 1);
            WaitForSingleObject(information.hProcess, 5000);
            CloseHandle(information.hThread);
            CloseHandle(information.hProcess);
            if (job != nullptr)
               CloseHandle(job);
            closeAll();
            result.error = ErrorText_(L"its job object could not be made", jobError);
            return false;
         }

         if (request.inspect)
            request.inspect(information.hProcess);

         result.started = true;
         ResumeThread(information.hThread);
         CloseHandle(information.hThread);

         // The request, all of it, then the end of the input. The pipe's buffer
         // holds it, so this does not wait for the child; a child that has already
         // gone fails the write, and its exit says the rest.
         {
            size_t offset = 0;
            while (offset < request.input.size())
            {
               DWORD written = 0;
               const DWORD chunk = (DWORD) std::min<size_t>(request.input.size() - offset, 1 << 20);
               if (!WriteFile(parentInput, request.input.data() + offset, chunk, &written, nullptr) || written == 0)
                  break;
               offset += written;
            }
            CloseHandle(parentInput);
            parentInput = nullptr;
         }

         // The answer, on a thread of its own, so that the wait below is the one
         // that decides: a child that writes nothing and never ends is killed at
         // the timeout, and its pipe breaks.
         std::string output;
         bool truncated = false;
         HANDLE reading = parentOutput;
         const size_t most = request.maximumOutputBytes;
         std::thread reader([&output, &truncated, reading, most]()
         {
            std::vector<char> buffer(16384);
            for (;;)
            {
               DWORD got = 0;
               if (!ReadFile(reading, buffer.data(), (DWORD) buffer.size(), &got, nullptr) || got == 0)
                  break;
               if (output.size() + got > most)
               {
                  output.append(buffer.data(), most - output.size());
                  truncated = true;
                  break;
               }
               output.append(buffer.data(), got);
            }
         });

         const DWORD waited = WaitForSingleObject(information.hProcess, request.timeoutMilliseconds);
         if (waited != WAIT_OBJECT_0)
         {
            result.timedOut = true;
            TerminateJobObject(job, 1);
            WaitForSingleObject(information.hProcess, 5000);
         }
         DWORD exitCode = 1;
         GetExitCodeProcess(information.hProcess, &exitCode);

         // Ends the child if it is still there, and breaks the pipe for the reader.
         CloseHandle(job);
         CloseHandle(information.hProcess);
         if (truncated)
            CancelIoEx(parentOutput, nullptr);
         reader.join();
         CloseHandle(parentOutput);
         parentOutput = nullptr;

         result.exitCode = exitCode;
         result.output = output;
         result.outputTruncated = truncated;
         return true;
      }
   }
}

#endif
