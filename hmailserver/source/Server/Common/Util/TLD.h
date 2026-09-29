// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class TLD : public Singleton<TLD>
   {
   public:

      TLD(void);
      ~TLD(void);

      void Initialize();
      bool IsTLD(const String &sName);

      bool GetDomainNameFromHost(String &sHost, bool &bIsIPAddress);

   private:

      std::set<String> tld_;

      // Guards tld_: Configuration::Load reads the list again at a reinitialise
      // while a spam test may be looking a name up in it. Held only to look up or
      // to swap in the list read - a leaf.
      boost::mutex tld_mutex_;
   };
}