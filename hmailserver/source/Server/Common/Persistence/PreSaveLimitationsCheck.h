// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "PersistenceMode.h"

namespace HM
{
   class Account;
   class Alias;
   class DistributionList;
   class Group;
   class Domain;
   class DomainAlias;
   class DistributionListRecipient;
   class Route;
   class RemoteDomainPolicy;
   class SecurityRange;

   class PreSaveLimitationsCheck
   {
   public:
      PreSaveLimitationsCheck(void);
      ~PreSaveLimitationsCheck(void);


      static bool CheckLimitations(PersistenceMode mode, std::shared_ptr<Domain> domain, String &resultDescription);
      static bool CheckLimitations(PersistenceMode mode, std::shared_ptr<DomainAlias> domainAlias, String &resultDescription);
      static bool CheckLimitations(PersistenceMode mode, std::shared_ptr<Account> account, String &resultDescription);
      static bool CheckLimitations(PersistenceMode mode, std::shared_ptr<Alias> alias, String &resultDescription);
      static bool CheckLimitations(PersistenceMode mode, std::shared_ptr<DistributionList> list, String &resultDescription);
      static bool CheckLimitations(PersistenceMode mode, std::shared_ptr<Group> group, String &resultDescription);
      static bool CheckLimitations(PersistenceMode mode, std::shared_ptr<DistributionListRecipient> pRecipient, String &resultDescription);
      static bool CheckLimitations(PersistenceMode mode, std::shared_ptr<Route> route, String &resultDescription);
      static bool CheckLimitations(PersistenceMode mode, std::shared_ptr<SecurityRange> route, String &resultDescription);
      static bool CheckLimitations(PersistenceMode mode, std::shared_ptr<RemoteDomainPolicy> policy, String &resultDescription);
    
      static bool IsValidAccountAddress_(const String &sEmailAddress);

      // A domain's mail is kept in the directory named after it directly under
      // the data directory, beside the directories the server keeps its own
      // stores in there: Protected, Confidential, Recoverable, Discovery,
      // Quarantine, Files, Lists, Blobs and the rest (the list is in the .cpp),
      // and the temp, events and database directories the Linux packages'
      // hMailServer.ini puts there. A domain whose name is one of them - a single
      // label, compared without regard to case - shares that directory with the
      // store: a backup leaves the mail of a domain named as a store it does not
      // carry out and a restore does not put it back, a store's clean-up removes
      // a mailbox directory it takes for one of its own, and renaming the domain
      // moved the whole store with it. True for such a name, with the directory
      // it collides with in collidesWith; a directory hMailServer.ini names
      // directly under the data directory counts as well.
      static bool IsReservedDomainName(const String &domainName, String &collidesWith);

      // Each domain whose name IsReservedDomainName answers true for - one made
      // before 6.3.4 refused such names, or put back by a restore - reported in
      // the error log, once each time the servers start. The first-hour
      // checklist says the same (SetupChecklist.cpp, domain_name_reserved).
      static void ReportReservedDomainNames();

   private:

      enum ObjectType
      {
         TypeAccount = 1,
         TypeAlias = 2,
         TypeList = 3,
         TypeGroup = 4
      };



      static std::shared_ptr<Domain> GetDomain(__int64 id);
      static bool DuplicateError(String &resultDescription);

      static bool GetDuplicateExist(std::shared_ptr<Domain> domain, ObjectType objectType, __int64 objectID, const String &objectName);
   };
}