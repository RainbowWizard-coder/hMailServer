// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "OutboundProviders.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      struct Suffix
      {
         OutboundProviders::Provider provider;
         const TCHAR *suffix;
      };

      // The table docs/IpWarmup.md prints. Fixed; see the header for why.
      // Several entries are covered by a shorter one beside them (every
      // aspmx.l.google.com host ends in google.com); they are written out
      // because they are the names an administrator reads in an MX lookup,
      // and the table is meant to be read as well as run.
      const Suffix Suffixes[] =
      {
         { OutboundProviders::Google,    _T("google.com") },
         { OutboundProviders::Google,    _T("googlemail.com") },
         { OutboundProviders::Google,    _T("aspmx.l.google.com") },
         { OutboundProviders::Microsoft, _T("outlook.com") },
         { OutboundProviders::Microsoft, _T("hotmail.com") },
         { OutboundProviders::Microsoft, _T("protection.outlook.com") },
         { OutboundProviders::Microsoft, _T("olc.protection.outlook.com") },
         { OutboundProviders::Yahoo,     _T("yahoodns.net") },
         { OutboundProviders::Yahoo,     _T("yahoo.com") },
         { OutboundProviders::Yahoo,     _T("aol.com") },
         { OutboundProviders::Apple,     _T("icloud.com") },
         { OutboundProviders::Apple,     _T("apple.com") },
         { OutboundProviders::Proton,    _T("protonmail.ch") },
         { OutboundProviders::Proton,    _T("proton.me") },
      };

      const TCHAR *Keys[OutboundProviders::Count] =
      {
         _T("google"), _T("microsoft"), _T("yahoo"), _T("apple"), _T("proton"), _T("other")
      };

      const TCHAR *DisplayNames[OutboundProviders::Count] =
      {
         _T("Google"), _T("Microsoft"), _T("Yahoo"), _T("Apple"), _T("Proton"), _T("other providers")
      };
   }

   OutboundProviders::Provider
   OutboundProviders::ClassifyHost(const String &hostName)
   {
      String host = hostName;
      host.Trim();
      host.ToLower();

      // A fully qualified name may carry the root's dot.
      while (host.EndsWith(_T(".")))
         host = host.Mid(0, host.GetLength() - 1);

      if (host.IsEmpty())
         return Other;

      for (const Suffix &entry : Suffixes)
      {
         const String suffix = entry.suffix;

         if (host == suffix)
            return entry.provider;

         // A dot before the suffix, so that notgoogle.com is nobody's.
         if (host.GetLength() > suffix.GetLength() && host.EndsWith(_T(".") + suffix))
            return entry.provider;
      }

      return Other;
   }

   String
   OutboundProviders::Key(Provider provider)
   {
      const int index = (int) provider;

      if (index < 0 || index >= Count)
         return Keys[Other];

      return Keys[index];
   }

   bool
   OutboundProviders::FromKey(const String &key, Provider &provider)
   {
      for (int index = 0; index < Count; index++)
      {
         if (key.CompareNoCase(Keys[index]) == 0)
         {
            provider = (Provider) index;
            return true;
         }
      }

      return false;
   }

   String
   OutboundProviders::DisplayName(Provider provider)
   {
      const int index = (int) provider;

      if (index < 0 || index >= Count)
         return DisplayNames[Other];

      return DisplayNames[index];
   }
}
