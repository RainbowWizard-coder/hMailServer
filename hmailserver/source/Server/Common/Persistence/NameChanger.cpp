// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "NameChanger.h"
#include "MessageStore.h"
#include "MessageStoreObjects.h"
#include "PreSaveLimitationsCheck.h"
#include "../Util/StorePath.h"
#include "../BO/Domain.h"
#include "../BO/Accounts.h"
#include "../BO/Aliases.h"
#include "../BO/DistributionLists.h"
#include "../BO/DistributionListRecipients.h"
#include "../BO/DistributionListRecipient.h"

#include "PersistentDistributionListRecipient.h"
#include "../Util/DistributionListMembership.h"
#include "../Sieve/SieveStorage.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   NameChanger::NameChanger()
   {

   }

   NameChanger::~NameChanger()
   {

   }

   bool
   NameChanger::RenameDomain(const String& oldDomainName, std::shared_ptr<Domain> pDomain, String &errorMessage)
   {
      const String& newDomainName = pDomain->GetName();

      // A domain whose name is one of the server's own directories under the
      // data directory shares that directory with the store: its mailboxes are
      // moved one by one, and the store's files stay where the store keeps them.
      String collidesWith;
      if (PreSaveLimitationsCheck::IsReservedDomainName(oldDomainName, collidesWith))
      {
         if (!MoveMailboxesOutOfReservedDirectory_(oldDomainName, pDomain, errorMessage))
            return false;
      }
      else if (!RenameDomainDataDirectory_(oldDomainName, pDomain->GetName(), errorMessage))
         return false;

      // Update accounts...
      std::vector<std::shared_ptr<Account> > vecAccounts = pDomain->GetAccounts()->GetSnapshot();
      auto iterAccount = vecAccounts.begin() ;
      auto iterAccountEnd = vecAccounts.end();

      for (; iterAccount != iterAccountEnd; iterAccount++)
      {
         std::shared_ptr<Account> pAccount = (*iterAccount);

         String sAddress = pAccount->GetAddress();
         UpdateDomainName_(sAddress, oldDomainName, newDomainName);
         pAccount->SetAddress(sAddress);

         String currentVal = pAccount->GetForwardAddress();
         if (UpdateDomainName_(currentVal, oldDomainName, newDomainName))
            pAccount->SetForwardAddress(currentVal);

         if (!PersistentAccount::SaveObject(pAccount, errorMessage, PersistenceMode::PersistenceModeRename))
            return false;
      }

      // Update aliases...
      std::vector<std::shared_ptr<Alias> > vecAliases = pDomain->GetAliases()->GetSnapshot();
      auto iterAlias = vecAliases.begin();
      auto iterAliasEnd = vecAliases.end();

      for (; iterAlias != iterAliasEnd; iterAlias++)
      {
         std::shared_ptr<Alias> pAlias = (*iterAlias);

         String sAddress = pAlias->GetName();
         UpdateDomainName_(sAddress, oldDomainName, newDomainName);
         pAlias->SetName(sAddress);

         String aliasValue = pAlias->GetValue();

         if (UpdateDomainName_(aliasValue, oldDomainName, newDomainName))
            pAlias->SetValue(aliasValue);

         if (!PersistentAlias::SaveObject(pAlias, errorMessage, PersistenceMode::PersistenceModeRename))
            return false;
      }

      // Update lists...
      std::vector<std::shared_ptr<DistributionList> > vecLists = pDomain->GetDistributionLists()->GetSnapshot();
      auto iterList = vecLists.begin();
      auto iterListEnd = vecLists.end();

      for (; iterList != iterListEnd; iterList++)
      {
         std::shared_ptr<DistributionList> pList = (*iterList);

         String sAddress = pList->GetAddress();
         UpdateDomainName_(sAddress, oldDomainName,newDomainName);
         pList->SetAddress(sAddress);

         std::vector<std::shared_ptr<HM::DistributionListRecipient>> recipients = pList->GetMembers()->GetSnapshot();
         for(std::shared_ptr<DistributionListRecipient> recipient : recipients)
         {
            String address = recipient->GetAddress();
            if (UpdateDomainName_(address,oldDomainName, newDomainName))
            {
               recipient->SetAddress(address);

               // Unchecked, while the list itself below is checked and fails the whole
               // rename - so the one asymmetry in this function was the one that leaves
               // a distribution list pointing at members in a domain that no longer
               // exists. Mail to the list then bounces for exactly those members, and
               // the rename that caused it completed successfully hours earlier.
               //
               // Failing the rename is right here rather than reporting and carrying
               // on: NameChanger's callers already treat false as "this rename did not
               // happen", and a half-renamed domain is the state worth avoiding.
               if (!PersistentDistributionListRecipient::SaveObject(recipient))
               {
                  // errorMessage is the out-parameter every other failure in this
                  // function fills in, and it is what the caller shows.
                  errorMessage = "The domain could not be renamed because the member '" + address +
                     "' of distribution list '" + pList->GetAddress() + "' could not be updated.";

                  return false;
               }
            }
         }

         if (!PersistentDistributionList::SaveObject(pList, errorMessage, PersistenceMode::PersistenceModeRename))
            return false;
      }

      // A membership query that names this domain - domain:<old name>, or a
      // pattern whose domain part is it - on a list of this domain or of any
      // other, follows the rename; left alone it would go on naming a domain
      // that no longer exists and answer nobody without saying so. Read from
      // the rows after the loop above has saved this domain's own lists, so
      // that nothing it rewrites is overwritten by an object held from before.
      if (!DistributionListMembership::RenameDomain(oldDomainName, newDomainName, errorMessage))
         return false;

      return true;

   }

   bool 
   NameChanger::UpdateDomainName_(String &sAddress, const String &oldDomainName, const String& newDomainName)
   {

      if (!sAddress.EndsWith("@" + oldDomainName))
      {
         // Doesn't seem to contain the domain name.
         return false;
      }

      int atPos = sAddress.Find(_T("@"));


      // Remove the current domain name.
      sAddress = sAddress.Mid(0, atPos+1);

      // Add the new one.
      sAddress += newDomainName;

      return true;
   }

   bool
   NameChanger::RenameAccount(const String& oldAccountName, std::shared_ptr<Account> pAccount, String &errorMessage)
   {
      String dataDirectory = IniFileSettings::Instance()->GetDataDirectory();
      String domainName = StoreDirectoryName(StringParser::ExtractDomain(oldAccountName));
      String domainDirectory = FileUtilities::Combine(dataDirectory, domainName);

      String oldMailboxName  = StoreDirectoryName(StringParser::ExtractAddress(oldAccountName));
      String newMailboxName  = StoreDirectoryName(StringParser::ExtractAddress(pAccount->GetAddress()));
      
      String oldDirectoryName = FileUtilities::Combine(domainDirectory, oldMailboxName);
      String newDirectoryName = FileUtilities::Combine(domainDirectory, newMailboxName);

      // The one caller is the domain rename's cascade (PersistentAccount::
      // SaveObject in PersistenceModeRename), which has moved the domain's
      // mailboxes and its Sieve tree already and keeps each local part: both
      // paths above are then the one directory under the old domain, where
      // nothing is left to move. RenameDirectory_ refuses a destination that
      // exists, so mail delivered there while the rename ran would have failed
      // the whole rename half-way; one directory is not moved onto itself.
      const bool sameMailboxDirectory = oldDirectoryName == newDirectoryName;
      if (!sameMailboxDirectory && !RenameDirectory_(oldDirectoryName, newDirectoryName, errorMessage))
         return false;

      // And the messages an object store keeps under the old name, which the
      // directory above did not hold (docs/ObjectStorage.md). A failure part way
      // is put back, objects and directory, since the rename is then refused and
      // the row keeps the old name; what the store will not give back stays
      // under the new name, never under neither, and the rename made again
      // carries the rest.
      unsigned int objectsMoved = 0;
      if (!MessageStore::MoveTree(oldDirectoryName, newDirectoryName, objectsMoved, errorMessage))
      {
         unsigned int movedBack = 0;
         String putBackProblem;
         if (objectsMoved > 0 && !MessageStore::MoveTree(newDirectoryName, oldDirectoryName, movedBack, putBackProblem))
            errorMessage += " Some of the objects moved before that could not be moved back (" + putBackProblem + "); they are under the new name until the rename is made again.";

         String ignored;
         RenameDirectory_(newDirectoryName, oldDirectoryName, ignored);
         MessageStoreObjects::CommitTree(oldDirectoryName);
         return false;
      }

      // The account's Sieve state does not live under its mailbox directory - it
      // has its own tree - so renaming the mailbox alone left the filter behind at
      // the old address, where nothing would ever read it again. The user's mail
      // then stopped being filed, discarded or redirected, silently.
      //
      // In the domain rename's cascade the domain's whole Sieve tree has been
      // moved before the accounts are walked (RenameDomainDataDirectory_,
      // MoveMailboxesOutOfReservedDirectory_), so the account's directory is at
      // the new address already and none is left at the old one. RenameDirectory_
      // refuses a destination that exists before it looks at the source, so for
      // every account with a Sieve script, a vacation reply's memory or a
      // duplicate check the rename failed there - after the mailboxes and the
      // Sieve tree had moved and before the row took the new name, which left
      // the domain's mail where its name did not look and a second rename
      // refused because the new directory existed. Nothing at the old address
      // is nothing to move.
      const String oldSieveDirectory = SieveStorage::GetAccountDirectory(oldAccountName);
      if (!FileUtilities::Exists(oldSieveDirectory))
         return true;

      return RenameDirectory_(oldSieveDirectory,
                              SieveStorage::GetAccountDirectory(pAccount->GetAddress()),
                              errorMessage);
   }

   bool 
   NameChanger::RenameDomainDataDirectory_(const String &oldDomainName, const String &newDomainName, String &errorMessage)
   {
      // Old director name
      String oldDirectory = FileUtilities::Combine(IniFileSettings::Instance()->GetDataDirectory(), StoreDirectoryName(oldDomainName));
      String newDirectory = FileUtilities::Combine(IniFileSettings::Instance()->GetDataDirectory(), StoreDirectoryName(newDomainName));

      if (!RenameDirectory_(oldDirectory, newDirectory, errorMessage))
         return false;

      // And the messages an object store keeps under the old domain, which the
      // directory above did not hold (docs/ObjectStorage.md). A failure part way
      // is put back as in RenameAccount.
      unsigned int objectsMoved = 0;
      if (!MessageStore::MoveTree(oldDirectory, newDirectory, objectsMoved, errorMessage))
      {
         unsigned int movedBack = 0;
         String putBackProblem;
         if (objectsMoved > 0 && !MessageStore::MoveTree(newDirectory, oldDirectory, movedBack, putBackProblem))
            errorMessage += " Some of the objects moved before that could not be moved back (" + putBackProblem + "); they are under the new name until the rename is made again.";

         String ignored;
         RenameDirectory_(newDirectory, oldDirectory, ignored);
         MessageStoreObjects::CommitTree(oldDirectory);
         return false;
      }

      // And the domain's Sieve tree, which lives outside the mailbox tree. Without
      // this a domain rename moved every mailbox and left every account's filter
      // behind: delivery looked the script up under the new domain, found nothing,
      // and delivered to INBOX everything the user had arranged to file, discard
      // or redirect - with no error and no log line.
      return RenameDirectory_(SieveStorage::GetDomainDirectory(oldDomainName),
                              SieveStorage::GetDomainDirectory(newDomainName),
                              errorMessage);
   }

   bool
   NameChanger::MoveMailbox_(const String &oldDirectory, const String &newDirectory, String &errorMessage)
   {
      if (!RenameDirectory_(oldDirectory, newDirectory, errorMessage))
         return false;

      // The messages an object store keeps under the old name, put back as in
      // RenameAccount when a part of them cannot be moved.
      unsigned int objectsMoved = 0;
      if (!MessageStore::MoveTree(oldDirectory, newDirectory, objectsMoved, errorMessage))
      {
         unsigned int movedBack = 0;
         String putBackProblem;
         if (objectsMoved > 0 && !MessageStore::MoveTree(newDirectory, oldDirectory, movedBack, putBackProblem))
            errorMessage += " Some of the objects moved before that could not be moved back (" + putBackProblem + "); they are under the new name until the rename is made again.";

         String ignored;
         RenameDirectory_(newDirectory, oldDirectory, ignored);
         MessageStoreObjects::CommitTree(oldDirectory);
         return false;
      }

      return true;
   }

   bool
   NameChanger::MoveMailboxesOutOfReservedDirectory_(const String &oldDomainName, std::shared_ptr<Domain> pDomain, String &errorMessage)
   {
      // The domain's directory is also a store's (PreSaveLimitationsCheck::
      // IsReservedDomainName): moving the whole of it, as a rename otherwise
      // does, took the store's files - the ciphertext behind every
      // password-protected message's link, say - into the renamed domain and
      // deleted them from where the store looks. Only the directories of the
      // domain's own accounts move, each as an account rename moves its own;
      // what else is in there stays.
      const String dataDirectory = IniFileSettings::Instance()->GetDataDirectory();
      const String oldDirectory = FileUtilities::Combine(dataDirectory, StoreDirectoryName(oldDomainName));
      const String newDirectory = FileUtilities::Combine(dataDirectory, StoreDirectoryName(pDomain->GetName()));

      if (FileUtilities::Exists(newDirectory))
      {
         errorMessage = Formatter::Format("Can't rename {0} to {1} since {1} already exists.", oldDirectory, newDirectory);
         return false;
      }

      std::vector<std::pair<String, String> > moved;
      bool failed = false;

      std::vector<std::shared_ptr<Account> > accounts = pDomain->GetAccounts()->GetSnapshot();
      for (std::shared_ptr<Account> account : accounts)
      {
         if (!account)
            continue;

         const String mailbox = StoreDirectoryName(StringParser::ExtractAddress(account->GetAddress()));
         if (mailbox.IsEmpty())
            continue;

         const String from = FileUtilities::Combine(oldDirectory, mailbox);
         const String to = FileUtilities::Combine(newDirectory, mailbox);

         if (!FileUtilities::Exists(newDirectory) && !FileUtilities::CreateDirectory(newDirectory))
         {
            errorMessage = Formatter::Format("Can't rename {0} to {1} since {1} could not be created.", oldDirectory, newDirectory);
            failed = true;
            break;
         }

         if (!MoveMailbox_(from, to, errorMessage))
         {
            failed = true;
            break;
         }

         moved.push_back(std::make_pair(from, to));
      }

      if (failed)
      {
         // The mailboxes moved before the failure go back, so that the rename,
         // refused, leaves every mailbox under the name the row keeps.
         for (auto entry = moved.rbegin(); entry != moved.rend(); ++entry)
         {
            String ignored;
            MoveMailbox_(entry->second, entry->first, ignored);
         }

         FileUtilities::DeleteDirectory(newDirectory, false);
         return false;
      }

      // And the domain's Sieve tree, which lives outside the mailbox tree, as
      // RenameDomainDataDirectory_ moves it.
      return RenameDirectory_(SieveStorage::GetDomainDirectory(oldDomainName),
                              SieveStorage::GetDomainDirectory(pDomain->GetName()),
                              errorMessage);
   }

   bool
   NameChanger::RenameDirectory_(const String &oldDirectory, const String &newDirectory, String &errorMessage)
   {
      if (FileUtilities::Exists(newDirectory))
      {
         errorMessage = Formatter::Format("Can't rename {0} to {1} since {1} already exists.", oldDirectory, newDirectory);
         return false;
      }

      if (!FileUtilities::Exists(oldDirectory))
      {
         // Nothing to rename.
         return true;
      }

      // Since renaming the directory is likely to fail (due to locked files and what not), we create a copy
      // of the entire directory and then delete the old one. Yes, this is slow as .. but it's more likely to
      // actually work.
      if (!FileUtilities::CopyDirectory(oldDirectory, newDirectory, errorMessage))
      {
         return false;
      }

      // We were successful in copying the data directory for the domain in question. Now drop the old structure.
      FileUtilities::DeleteDirectory(oldDirectory, true);

      return true;
   }
}

