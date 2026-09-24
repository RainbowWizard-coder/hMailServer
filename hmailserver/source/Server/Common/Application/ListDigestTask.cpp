// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "ListDigestTask.h"

#include "../Util/ListArchive.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Runs since the last sweep. Started high so that the first pass after a
      // start sweeps: lapsed requests are not worth an hour's wait.
      int runs_since_sweep = 12;
   }

   ListDigestTask::ListDigestTask()
   {
   }

   ListDigestTask::~ListDigestTask()
   {
   }

   void
   ListDigestTask::DoWork()
   {
      if (++runs_since_sweep >= 12)
      {
         runs_since_sweep = 0;
         ListArchive::Sweep();
      }

      ListArchive::RunDigests();
   }
}
