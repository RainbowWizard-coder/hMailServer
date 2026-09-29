// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   class LocalIPAddresses : public Singleton<LocalIPAddresses>
   {
   public:
      LocalIPAddresses();
      virtual ~LocalIPAddresses();

      void LoadIPAddresses();

      bool IsLocalIPAddress(const IPAddress &address);
      bool IsLocalPort(const IPAddress &address, int port);
      bool IsWithinLoopbackRange(const IPAddress &address);

   private:
      
      std::vector<std::pair<IPAddress, int> > local_ports_;

      // Guards local_ports_: LoadIPAddresses fills it again at every start of the
      // servers and every reinitialise (Configuration::Load) while other threads
      // walk it - COM's Utilities.IsLocalHost, and a connection being made. Held
      // only to walk it or to swap in the list built - a leaf.
      boost::mutex local_ports_mutex_;
   };


   class LocalIPAddressesTester
   {
   public :
      LocalIPAddressesTester () {};
      ~LocalIPAddressesTester () {};      

      void Test();
   };



}
