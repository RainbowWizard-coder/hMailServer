// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "Backup.h"

#include "../Util/Utilities.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   Backup::Backup(void)
   {
      contains_ = 0;
      restore_options_ = 0;
   }

   Backup::~Backup(void)
   {
      // The passphrase goes with the object rather than being left in the freed
      // block. This one is held for as long as whoever asked for the restore
      // holds the object - over COM that is the Control Panel's Backup object,
      // which lives until it is released - and the server writes minidumps on a
      // crash by design. See Utilities::SecureWipe for what that does and does
      // not claim.
      Utilities::SecureWipe(passphrase_);
   }

   bool 
   Backup::GetContains(int iOption) const
   {
      return (contains_ & iOption) ? true : false;      
   }

   int 
   Backup::GetRestoreOption(int iRestoreOption) const
   {
      return (restore_options_ & iRestoreOption) ? true : false;      
   }

   void 
   Backup::SetRestoreOption(int iOption, bool bSet)
   {
      if (bSet)
         restore_options_ = restore_options_ | iOption;
      else
         restore_options_ = restore_options_ &~ iOption;

   }

}