// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class FolderManipulationLock
   {
   public:
      FolderManipulationLock(int iAccountID, int iFolderID);
      ~FolderManipulationLock(void);
      // The destructor releases the folder the lock holds: a copy would release
      // it a second time, perhaps while another thread holds it.
      FolderManipulationLock(const FolderManipulationLock &) = delete;
      FolderManipulationLock &operator=(const FolderManipulationLock &) = delete;

      void Lock();

   private:

      static void Acquire(std::pair<int, int> lockPair);
      static void Release(std::pair<int, int> lockPair);

      static std::set<std::pair<int, int> > folders_;
      
      static boost::recursive_mutex mutex_; 

      bool has_lock_;

      std::pair<int, int> lock_pair_;
   };
}