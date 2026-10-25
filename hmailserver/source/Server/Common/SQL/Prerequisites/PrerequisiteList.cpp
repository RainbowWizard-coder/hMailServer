// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"
#include "PrerequisiteList.h"

#include "PreReqNoDuplicateFolders.h"
#include "PreReqGreyListKeys.h"
#include "IPrerequisite.h"


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   PrerequisiteList::PrerequisiteList(void)
   {
      Initialize_();
   }

   PrerequisiteList::~PrerequisiteList(void)
   {
   }

   void
   PrerequisiteList::Initialize_()
   {
      std::shared_ptr<PreReqNoDuplicateFolders> duplicateFolderCheck = std::shared_ptr<PreReqNoDuplicateFolders>(new PreReqNoDuplicateFolders);
      prerequisites_.push_back(std::make_pair(duplicateFolderCheck->GetDatabaseVersion(), duplicateFolderCheck));

      std::shared_ptr<PreReqGreyListKeys> greyListKeys = std::shared_ptr<PreReqGreyListKeys>(new PreReqGreyListKeys);
      prerequisites_.push_back(std::make_pair(greyListKeys->GetDatabaseVersion(), greyListKeys));
   }

   bool
   PrerequisiteList::Ensure(std::shared_ptr<DALConnection> connection, int scriptDatabaseVersion, String &sErrorMessage)
   {
      return Ensure_(connection, scriptDatabaseVersion, false, PrerequisiteReport(), sErrorMessage);
   }

   bool
   PrerequisiteList::Ensure(std::shared_ptr<DALConnection> connection, int scriptDatabaseVersion, const PrerequisiteReport &report, String &sErrorMessage)
   {
      return Ensure_(connection, scriptDatabaseVersion, false, report, sErrorMessage);
   }

   bool
   PrerequisiteList::EnsureBeforeTransaction(std::shared_ptr<DALConnection> connection, int scriptDatabaseVersion, const PrerequisiteReport &report, String &sErrorMessage)
   {
      return Ensure_(connection, scriptDatabaseVersion, true, report, sErrorMessage);
   }

   bool
   PrerequisiteList::Ensure_(std::shared_ptr<DALConnection> connection, int scriptDatabaseVersion, bool beforeTransactionOnly, const PrerequisiteReport &report, String &sErrorMessage)
   {
      auto iter = prerequisites_.begin();
      auto iterEnd = prerequisites_.end();

      for (; iter != iterEnd; iter++)
      {
         int databaseVersion = (*iter).first;

         if (databaseVersion == scriptDatabaseVersion)
         {
            std::shared_ptr<IPrerequisite > prereq = (*iter).second;

            if (beforeTransactionOnly && !prereq->RunsBeforeTransaction())
               continue;

            if (!prereq->Ensure(connection, report, sErrorMessage))
               return false;
         }
      }

      return true;
   }


}
