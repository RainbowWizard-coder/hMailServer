#pragma once

// Build57B.0: explicit Sent ownership policy.
// No content-based matching, deletion or UID manipulation.

#include <windows.h>
#include <memory>
#include <string>

#include "../BO/Account.h"

namespace HM
{
   class SentClientPolicy57B
   {
   public:
      static bool ClientOwnsSent(
         const std::shared_ptr<const Account>& account)
      {
         if (!account || account->GetID() <= 0)
            return false;

         wchar_t modulePath[MAX_PATH] = {};
         const DWORD length =
            GetModuleFileNameW(nullptr, modulePath, MAX_PATH);

         if (length == 0 || length >= MAX_PATH)
            return false;

         std::wstring iniPath(modulePath, length);
         const size_t slash = iniPath.find_last_of(L"\\/");

         if (slash == std::wstring::npos)
            return false;

         iniPath.replace(
            slash + 1,
            std::wstring::npos,
            L"hMailServer.ini");

         const DWORD attributes =
            GetFileAttributesW(iniPath.c_str());

         if (attributes == INVALID_FILE_ATTRIBUTES ||
             (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            return false;

         const std::wstring key =
            std::to_wstring(
               static_cast<long long>(account->GetID()));

         // Only explicit value 1 selects client-managed Sent.
         // Missing, invalid or unreadable policy defaults to
         // the existing server-managed behavior.
         const UINT value = GetPrivateProfileIntW(
            L"SentClientManagedSent",
            key.c_str(),
            0,
            iniPath.c_str());

         return value == 1;
      }
   };
}