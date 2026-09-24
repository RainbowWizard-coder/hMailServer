// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "PreSaveLimitationsCheck.h"

#include "../BO/Domain.h"
#include "../BO/DomainAlias.h"
#include "../BO/DomainAliases.h"
#include "../BO/Accounts.h"
#include "../BO/Aliases.h"
#include "../BO/DistributionLists.h"
#include "../BO/DistributionListRecipient.h"
#include "../Util/DistributionListMembership.h"
#include "../BO/Groups.h"
#include "../BO/Route.h"
#include "../BO/Routes.h"
#include "../BO/RemoteDomainPolicy.h"
#include "../BO/RemoteDomainPolicies.h"
#include "../BO/SecurityRanges.h"

#include "../Cache/CacheContainer.h"
#include "../AntiSpam/SpamTrapMonitor.h"
#include "../Application/ObjectCache.h"

#include "PersistentDomain.h"

#include "../../IMAP/IMAPConfiguration.h"
#include "../../SMTP/SMTPConfiguration.h"

#include "../Util/RegularExpression.h"
#include "../Util/GatewayEncryption.h"
#include "../Util/Unicode.h"
#include "PersistentRemoteDomainPolicy.h"

#include <ctime>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM   
{
   PreSaveLimitationsCheck::PreSaveLimitationsCheck()
   {
   }

   PreSaveLimitationsCheck::~PreSaveLimitationsCheck()
   {

   }

   std::shared_ptr<Domain> 
   PreSaveLimitationsCheck::GetDomain(__int64 id)
   {
      std::shared_ptr<Domain> domain = std::shared_ptr<Domain>(new Domain);

      PersistentDomain::ReadObject(domain, id);

      return domain;
   }

   bool
   PreSaveLimitationsCheck::IsValidAccountAddress_(const String &sEmailAddress)
   {
      // Stricter than general email validation: additionally forbids \ / ? * | :
      // in the local part because hMailServer uses the account address to build
      // filesystem paths for message storage, and these characters are illegal
      // in Windows file/directory names.
      //
      // The colon was missing from that set until 15 August 2026, and it belongs there
      // for exactly the same reason as the other five rather than a special one. The
      // local part becomes a DIRECTORY component - PersistentMessage::GetFileName
      // builds <data>\<domain>\<local part>\<2 guid chars>\<guid>.eml - and
      // CreateDirectory refuses a name containing a colon outright ("The directory name
      // is invalid"), measured on Windows 11 rather than assumed. So an account at
      // "a:b@example.com" saves, appears in every list, accepts mail at RCPT TO, and
      // then cannot have its folder created when the message is filed. It is a mailbox
      // that can never work, created without complaint.
      //
      // Worth being exact about one thing this does NOT do, because the colon's other
      // reputation invites the mistake: it is not stopping an alternate-data-stream
      // trick. A colon opens an ADS only where the component is a FILE name; here it is
      // an intermediate directory, two components above the file, so the failure is a
      // plain refusal rather than a hidden stream.
      //
      // PersistenceModeRestore and PersistenceModeRename return before this is reached,
      // so an installation that somehow holds such an address can still be restored and
      // renamed. Anything else is now refused - not only creating a new one, but SAVING
      // an existing one, because a normal save revalidates the address. That is the
      // right trade (the account cannot receive mail either way) but it does mean such
      // an account must be renamed rather than edited.

      const int maxEmailAddressLength = 254;
      if (sEmailAddress.GetLength() > maxEmailAddressLength)
         return false;

      // Note: RFC 5321 Section 4.5.3.1.1 limits the local part to 64 octets, but we
      // intentionally do not enforce this to maintain backwards compatibility with
      // existing accounts that have longer local parts.
      //
      // Original: ^(("[^<>@\\/\?\*|:]+")|(?!\.|.*\.(\.|@))[^<> @\\/"\?\*|:]+)@(\[([0-9]{1,3}\.){3}[0-9]{1,3}\]|\[IPv6:(?:[A-Fa-f0-9]{1,4}:){7}[A-Fa-f0-9]{1,4}\]|(?=.{1,255}$)((?!-|\.)[a-zA-Z0-9-]{0,62}[a-zA-Z0-9])(|\.(?!-|\.)[a-zA-Z0-9-]{0,62}[a-zA-Z0-9]){1,126})$
      //
      // The colon is excluded from the two LOCAL-PART classes only. It still appears
      // twice in the domain alternation - once in the "[IPv6:" literal and once in the
      // group matching the address itself - and must stay there, or an IPv6 address
      // literal would stop being a legal domain.
      //
      // That is a deliberate narrowing, not an oversight, and it is worth naming the
      // gap it leaves: the domain DOES reach the store path (GetFileName's first
      // component is the domain name), so an account in a domain literally named
      // "[IPv6:...]" would hit the same unbuildable-path problem. It cannot arise here,
      // because CheckLimitations already requires the account's domain to match the
      // owning hMailServer domain and IsValidDomainName refuses bracketed literals for
      // a domain object - so there is no path by which such an account exists to be
      // saved. If domain creation ever loosens, this is the second place to look.
      //
      // Conversion:
      // 1) Replace \ with \\
      // 2) Replace " with \"

      String regularExpression = "^((\"[^<>@\\\\/\\?\\*|:]+\")|(?!\\.|.*\\.(\\.|@))[^<> @\\\\/\"\\?\\*|:]+)@(\\[([0-9]{1,3}\\.){3}[0-9]{1,3}\\]|\\[IPv6:(?:[A-Fa-f0-9]{1,4}:){7}[A-Fa-f0-9]{1,4}\\]|(?=.{1,255}$)((?!-|\\.)[a-zA-Z0-9-]{0,62}[a-zA-Z0-9])(|\\.(?!-|\\.)[a-zA-Z0-9-]{0,62}[a-zA-Z0-9]){1,126})$";

      RegularExpression regexpEvaluator;
      return regexpEvaluator.TestExactMatch(regularExpression, sEmailAddress);
   }

   bool
   PreSaveLimitationsCheck::CheckLimitations(PersistenceMode mode, std::shared_ptr<Account> account, String &resultDescription)
   {
      if (mode == PersistenceModeRestore || mode == PersistenceModeRename)
         return true;

      if (account->GetVacationMessage().GetLength() > 1000)
      {
         resultDescription = "The auto reply message length exceeds the 1000 character limit.";
         return false;
      }

      // The account's other fixed-width text columns, checked here for the same reason
      // as the auto reply message above. Without a check the over-long value travels
      // all the way to the database driver, the driver refuses the row, and
      // PersistentAccount::SaveObject returns false with resultDescription still EMPTY.
      // An empty reason is all a caller has to go on: RestApiRoutes turns a non-empty
      // one into a 400 carrying the sentence and an empty one into 500 "failed to save
      // account", so PUT /api/v1/accounts blames the server for the caller's
      // 8,000-character first name, and the COM Save() raises a failure with nothing an
      // administrator can read. RegressionTests/Stress/StabilitySanityTests.cs,
      // TestInsertionOfTooLongString, is exactly that case.
      //
      // MySQL truncates instead of failing only where STRICT_TRANS_TABLES has been
      // switched off - the default before MySQL 5.7.5 and MariaDB 10.2.4, and still
      // what an administrator gets who has cleared sql_mode by hand. On a modern
      // default-configured server it is error 1406, "Data too long for column". The
      // 2010-era comment in that test records the truncation as a possibility rather
      // than a certainty, and no MySQL server was measured for this change - so
      // the identical save "succeeds" with a shortened name on one backend and fails
      // with no explanation on another. Checking before the save makes the answer the
      // same on every backend, and makes it a sentence.
      //
      // The widths below were read from the create scripts, not assumed:
      // DBScripts/CreateTablesMSSQL.sql, CreateTablesMySQL.sql and CreateTablesPGSQL.sql.
      // There is no CreateTablesMSSQLCE.sql - a Compact Edition database is built from
      // the MSSQL script with " varchar" rewritten to " nvarchar" (the create path in
      // build/check-db-scripts.ps1 reproduces what a fresh install runs), so Compact
      // Edition's widths are MSSQL's. All seven of these columns carry the SAME width in
      // all four backends, so none of them sets a narrower limit than the others. Where
      // they do differ the narrowest has to win, or a save that works on one backend
      // fails on the next; the auto reply message above is the example - nvarchar(1000)
      // on MSSQL against text on MySQL and PostgreSQL - which is where its 1000 comes
      // from.
      //
      // Deliberately absent: accountsignatureplaintext and accountsignaturehtml are
      // ntext on MSSQL and text on MySQL and PostgreSQL, fixed width in none of them, so
      // any limit invented here would refuse a signature the database would have stored.
      // accountvacationmessage is likewise text on two of the three, and is covered
      // above by the MSSQL width rather than by a guess.

      if (account->GetAddress().GetLength() > 255)
      {
         // accountaddress holds 255, one more than the 254 IsValidAccountAddress_
         // enforces just below. It is still worth testing first: a caller who sends a
         // 300-character address should be told the address is too long, rather than
         // that it is not a valid email address.
         resultDescription = "The account address length exceeds the 255 character limit.";
         return false;
      }

      if (account->GetVacationSubject().GetLength() > 200)
      {
         resultDescription = "The auto reply subject length exceeds the 200 character limit.";
         return false;
      }

      if (account->GetForwardAddress().GetLength() > 255)
      {
         resultDescription = "The forward address length exceeds the 255 character limit.";
         return false;
      }

      if (account->GetPersonFirstName().GetLength() > 60)
      {
         resultDescription = "The first name length exceeds the 60 character limit.";
         return false;
      }

      if (account->GetPersonLastName().GetLength() > 60)
      {
         resultDescription = "The last name length exceeds the 60 character limit.";
         return false;
      }

      if (account->GetADDomain().GetLength() > 255)
      {
         resultDescription = "The Active Directory domain length exceeds the 255 character limit.";
         return false;
      }

      if (account->GetADUsername().GetLength() > 255)
      {
         resultDescription = "The Active Directory user name length exceeds the 255 character limit.";
         return false;
      }

      if (!IsValidAccountAddress_(account->GetAddress()))
      {
         resultDescription = "The account address is not a valid email address.";
         return false;
      }

      // hMailServer does nopt support spaces or quotes in local accounts.
      if (account->GetAddress().FindOneOf(_T(" \"")) != -1)
      {
         resultDescription = "The account address may not contain spaces or quotes.";
         return false;
      }

      std::shared_ptr<Domain> domain = GetDomain(account->GetDomainID());

      if (GetDuplicateExist(domain, TypeAccount, account->GetID(), account->GetAddress()))      
         return DuplicateError(resultDescription);

      auto domainName = StringParser::ExtractDomain(account->GetAddress());
      if (domainName.CompareNoCase(domain->GetName()) != 0)
      {
         resultDescription = "The account address domain does not match the owning domain name.";
         return false;
      }

      // An address that is one of the domain's spam traps is not an address to
      // give to anybody. The trap exists so that mail to it is proof of a
      // harvested list; a mailbox, an alias or a list there would take delivery
      // of what was aimed at the trap and destroy the evidence with it. The trap
      // has to be removed first, which is a decision somebody makes rather than
      // something that happens quietly. Every save comes through here - COM, REST,
      // the import tool and a directory sync alike - except a restore and a
      // rename, which have returned above.
      //
      // Asked AFTER the address is known to belong to this domain, and that order
      // matters: IsTrapAddress answers about the domain in the ADDRESS, not the
      // domain being saved into, so asking first refused an address in somebody
      // else's domain with a sentence about "this domain's spam traps" - which is
      // both wrong and a quiet disclosure that the other domain traps it.
      if (SpamTrapMonitor::IsTrapAddress(account->GetAddress()))
      {
         resultDescription = "That address is one of this domain's spam traps. Remove the trap before giving the address to an account, an alias or a distribution list.";
         return false;
      }

      if (account->GetID() == 0)
      {
         if (domain->GetMaxNoOfAccountsEnabled() && 
             domain->GetAccounts()->GetCount() >= domain->GetMaxNoOfAccounts())
         {
            resultDescription = "The maximum number of accounts have been created.";
            return false;
         }
      }

      if (domain->GetMaxAccountSize() != 0)
      {
         if (account->GetAccountMaxSize() > domain->GetMaxAccountSize())
         {
            resultDescription = "The account is larger than the maximum account size specified in the domain settings.";
            return false;
         }

         if (account->GetAccountMaxSize() == 0)
         {
            resultDescription = "The domain has a maximum account size set. When this is the case, all accounts in the domains must have an account size set.";
            return false;
         }
      }

      if (domain->GetMaxSizeMB() > 0)
      {

         if (account->GetAccountMaxSize() == 0)
         {
            resultDescription = "The domain has a maximum account size set. When this is the case, all accounts in the domains must have an account size set.";
            return false;
         }

         String sError = "Account could not be saved. The total size of all accounts in the domain would exceed the maximum size for the domain.";
         
         __int64 currentSize = PersistentDomain::GetAllocatedSize(domain);

         if (account->GetID() == 0)
         {
            if (currentSize + account->GetAccountMaxSize() > domain->GetMaxSizeMB())
            {
               resultDescription = sError;
               return false;
            }
         }
         else
         {
            std::shared_ptr<Account> currentAccountSettings = std::shared_ptr<Account>(new Account);
            PersistentAccount::ReadObject(currentAccountSettings, account->GetID());

            if (currentSize - currentAccountSettings->GetAccountMaxSize() + account->GetAccountMaxSize() > domain->GetMaxSizeMB())
            {
               resultDescription = sError;
               return false;
            }
         }
      }

      String address = account->GetAddress().ToLower();
      if (address.Find(_T("\\")) >= 0 || address.Find(_T("/")) >= 0)
      {
         resultDescription = "An account name may not contain the characters \\ or /.";
         return false;
      }


      return true;
   }

   bool
   PreSaveLimitationsCheck::CheckLimitations(PersistenceMode mode, std::shared_ptr<DistributionListRecipient> recipient, String &resultDescription)
   {
      if (mode == PersistenceModeRestore || mode == PersistenceModeRename)
         return true;

      if (recipient->GetAddress().GetLength() == 0)
      {
         resultDescription = "The recipient address is empty";
         return false;
      }

      return true;
   }

   bool
   PreSaveLimitationsCheck::CheckLimitations(PersistenceMode mode, std::shared_ptr<Alias> alias, String &resultDescription)
   {
      if (mode == PersistenceModeRestore || mode == PersistenceModeRename)
         return true;

      std::shared_ptr<Domain> domain = GetDomain(alias->GetDomainID());

      if (GetDuplicateExist(domain, TypeAlias, alias->GetID(), alias->GetName()))      
         return DuplicateError(resultDescription);

      auto domainName = StringParser::ExtractDomain(alias->GetName());
      if (domainName.CompareNoCase(domain->GetName()) != 0)
      {
         resultDescription = "The alias address domain does not match the owning domain name.";
         return false;
      }

      // An address that is one of the domain's spam traps is not an address to
      // give to anybody. The trap exists so that mail to it is proof of a
      // harvested list; a mailbox, an alias or a list there would take delivery
      // of what was aimed at the trap and destroy the evidence with it. The trap
      // has to be removed first, which is a decision somebody makes rather than
      // something that happens quietly. Every save comes through here - COM, REST,
      // the import tool and a directory sync alike - except a restore and a
      // rename, which have returned above.
      //
      // Asked AFTER the address is known to belong to this domain, and that order
      // matters: IsTrapAddress answers about the domain in the ADDRESS, not the
      // domain being saved into, so asking first refused an address in somebody
      // else's domain with a sentence about "this domain's spam traps" - which is
      // both wrong and a quiet disclosure that the other domain traps it.
      if (SpamTrapMonitor::IsTrapAddress(alias->GetName()))
      {
         resultDescription = "That address is one of this domain's spam traps. Remove the trap before giving the address to an account, an alias or a distribution list.";
         return false;
      }

      if (alias->GetID() == 0)
      {
         if (domain->GetMaxNoOfAliasesEnabled() && 
             domain->GetAliases()->GetCount() >= domain->GetMaxNoOfAliases())
         {
            resultDescription = "The maximum number of aliases have been created.";
            return false;
         }
      }


      return true;
   }

   bool
   PreSaveLimitationsCheck::CheckLimitations(PersistenceMode mode, std::shared_ptr<DistributionList> list, String &resultDescription)
   {
      if (mode == PersistenceModeRestore || mode == PersistenceModeRename)
         return true;

      std::shared_ptr<Domain> domain = GetDomain(list->GetDomainID());

      if (GetDuplicateExist(domain, TypeList,list->GetID(), list->GetAddress()))      
         return DuplicateError(resultDescription);

      auto domainName = StringParser::ExtractDomain(list->GetAddress());
      if (domainName.CompareNoCase(domain->GetName()) != 0)
      {
         resultDescription = "The distribution list domain does not match the owning domain name.";
         return false;
      }

      // An address that is one of the domain's spam traps is not an address to
      // give to anybody. The trap exists so that mail to it is proof of a
      // harvested list; a mailbox, an alias or a list there would take delivery
      // of what was aimed at the trap and destroy the evidence with it. The trap
      // has to be removed first, which is a decision somebody makes rather than
      // something that happens quietly. Every save comes through here - COM, REST,
      // the import tool and a directory sync alike - except a restore and a
      // rename, which have returned above.
      //
      // Asked AFTER the address is known to belong to this domain, and that order
      // matters: IsTrapAddress answers about the domain in the ADDRESS, not the
      // domain being saved into, so asking first refused an address in somebody
      // else's domain with a sentence about "this domain's spam traps" - which is
      // both wrong and a quiet disclosure that the other domain traps it.
      if (SpamTrapMonitor::IsTrapAddress(list->GetAddress()))
      {
         resultDescription = "That address is one of this domain's spam traps. Remove the trap before giving the address to an account, an alias or a distribution list.";
         return false;
      }

      // The membership query, judged here so that COM's Save and the REST
      // update refuse the same text in the same sentence, and stored in its
      // one canonical spelling. A restore and a rename have returned above:
      // a backup carries what was valid when it was taken, and a domain the
      // query names may not have been restored yet.
      {
         String normalised, reason;
         if (!DistributionListMembership::Validate(list->GetMembership(), normalised, reason))
         {
            resultDescription = reason;
            return false;
         }

         list->SetMembership(normalised);
      }

      // The list manager's fields (ListManager.h), judged here for the same
      // reason: COM's Save and the REST routes refuse a digest hour of 25 or a
      // prefix of 65 characters in the one sentence.
      {
         String reason;
         if (!list->ValidateManager(reason))
         {
            resultDescription = reason;
            return false;
         }
      }

      if (list->GetID() == 0)
      {
         // The flag, not the number. The Control Panel draws this limit as a
         // checkbox beside a number - DomainDialog.cs writes both
         // MaxNumberOfDistributionListsEnabled and MaxNumberOfDistributionLists -
         // and leaves the number alone when the box is cleared, so testing the
         // number meant clearing the box did nothing at all: the old number went on
         // being enforced. Ticking it with the number at 0 enforced nothing. The
         // account and alias checks above both test their flag. Both columns
         // arrived in the same schema step (Upgrade4301to4400MSSQL.sql), so no
         // upgraded database can hold a number without the flag - the only way to
         // reach the broken state is for an administrator to switch the limit off.
         if (domain->GetMaxNoOfDistributionListsEnabled() &&
             domain->GetDistributionLists()->GetCount() >= domain->GetMaxNoOfDistributionLists())
         {
            resultDescription = "The maximum number of distribution lists have been created.";
            return false;
         }
      }

      return true;
   }

   bool
   PreSaveLimitationsCheck::CheckLimitations(PersistenceMode mode, std::shared_ptr<Group> group, String &resultDescription)
   {
      if (mode == PersistenceModeRestore || mode == PersistenceModeRename)
         return true;

      std::shared_ptr<Group> pGroup = Configuration::Instance()->GetIMAPConfiguration()->GetGroups()->GetItemByName(group->GetName());

      if (pGroup && (group->GetID() == 0 || group->GetID() != pGroup->GetID()))
      {
         resultDescription = "Another group with this name already exists.";
         return false;
      }

      return true;
   }

   bool
   PreSaveLimitationsCheck::DuplicateError(String &resultDescription)
   {
      resultDescription = "Another object with the same name already exists in this domain.";
      return false;
   }

   bool 
   PreSaveLimitationsCheck::GetDuplicateExist(std::shared_ptr<Domain> domain, ObjectType objectType, __int64 objectID, const String &objectName)
   {

      std::shared_ptr<Account> pAccount = std::shared_ptr<Account>(new Account);
      if (PersistentAccount::ReadObject(pAccount, objectName))
      {
         if (pAccount && (pAccount->GetID() != objectID || objectType != TypeAccount) )
            return true;
      }

      std::shared_ptr<Alias> pAlias = std::shared_ptr<Alias>(new Alias);
      if (PersistentAlias::ReadObject(pAlias, objectName))
      {
         if (pAlias && (pAlias->GetID() != objectID || objectType != TypeAlias))
            return true;
      }

      std::shared_ptr<DistributionList> pList = std::shared_ptr<DistributionList>(new DistributionList);; 
      if (PersistentDistributionList::ReadObject(pList,objectName ))
      {
         if (pList && (pList->GetID() != objectID || objectType != TypeList))
            return true;
      }

      return false;
   }

   bool
   PreSaveLimitationsCheck::CheckLimitations(PersistenceMode mode, std::shared_ptr<Domain> domain, String &resultDescription)
   {
      if (mode == PersistenceModeRestore || mode == PersistenceModeRename)
         return true;

      std::shared_ptr<const Domain> pDomain = CacheContainer::Instance()->GetDomain(domain->GetName());

      if (pDomain && (domain->GetID() == 0 || domain->GetID() != pDomain->GetID()))
      {
         resultDescription = "Another domain with this name already exists.";
         return false;
      }

      String sName = domain->GetName();
      if (!StringParser::IsValidDomainName(sName))
      {
         resultDescription = "The domain name you have entered is not a valid domain name.";
         return false;
      }

      // Check if there's a domain alias with this name. If so, this domain would be a duplciate.
      std::shared_ptr<DomainAliases> pDomainAliases = ObjectCache::Instance()->GetDomainAliases();
      std::shared_ptr<DomainAlias> pDomainAlias = pDomainAliases->GetItemByName(domain->GetName());
      if (pDomainAlias)
      {
         resultDescription = "A domain alias with this name already exists.";
         return false;
      }

      // If plus addressing is enabled, a plus addressing character must be selected.
      if (domain->GetUsePlusAddressing() && domain->GetPlusAddressingChar() == _T(""))
      {
         resultDescription = "A plus addressing character must be selected when enabling plus addressing.";
         return false;
      }
   
      return true;
   }

   bool
   PreSaveLimitationsCheck::CheckLimitations(PersistenceMode mode, std::shared_ptr<DomainAlias> domainAlias, String &resultDescription)
   {
      if (mode == PersistenceModeRestore || mode == PersistenceModeRename)
         return true;


      std::shared_ptr<const Domain> pDomain = CacheContainer::Instance()->GetDomain(domainAlias->GetName());

      if (pDomain)
      {
         resultDescription = "Another domain with this name already exists.";
         return false;
      }

      // Check if there's a domain alias with this name. If so, this domain would be a duplciate.
      std::shared_ptr<DomainAliases> pDomainAliases = ObjectCache::Instance()->GetDomainAliases();
      std::shared_ptr<DomainAlias> pDomainAlias = pDomainAliases->GetItemByName(domainAlias->GetName());
      if (pDomainAlias && (domainAlias->GetID() == 0 || domainAlias->GetID() != pDomainAlias->GetID()))
      {
         resultDescription = "A domain alias with this name already exists.";
         return false;
      }

      return true;
   }

   bool 
   PreSaveLimitationsCheck::CheckLimitations(PersistenceMode mode, std::shared_ptr<Route> route, String &resultDescription)
   {
      if (mode == PersistenceModeRestore || mode == PersistenceModeRename)
         return true;

      std::shared_ptr<Routes> pRoutes = Configuration::Instance()->GetSMTPConfiguration()->GetRoutes();

      std::shared_ptr<Route> existingRoute = pRoutes->GetItemByName(route->GetName());
      if (existingRoute && existingRoute->GetID() != route->GetID())
      {
        resultDescription = "Another route with this name already exists.";
        return false;  
      }
 
      return true;
   }

   bool
   PreSaveLimitationsCheck::CheckLimitations(PersistenceMode mode, std::shared_ptr<RemoteDomainPolicy> policy, String &resultDescription)
   {
      if (mode == PersistenceModeRestore || mode == PersistenceModeRename)
         return true;

      if (policy->GetDomainName().IsEmpty())
      {
         resultDescription = "A remote domain policy must name a domain or a pattern.";
         return false;
      }

      // One record per pattern. Two rows with the same pattern would both match
      // every domain it covers and be equally specific, so which one governed a
      // delivery would come down to the id - a policy decided by insertion order,
      // which is the thing GetPolicyForDomain exists to avoid.
      std::shared_ptr<RemoteDomainPolicies> policies =
         Configuration::Instance()->GetSMTPConfiguration()->GetRemoteDomainPolicies();

      if (policies)
      {
         std::shared_ptr<RemoteDomainPolicy> existing = policies->GetItemByName(policy->GetName());
         if (existing && existing->GetID() != policy->GetID())
         {
            resultDescription = "Another remote domain policy for this domain already exists.";
            return false;
         }
      }

      // Gateway encryption: the partner's key.
      //
      // White space around it is not part of it, and white space alone is no
      // key: the key is stored trimmed, on every surface (REST, COM, the
      // Control Panel, the Deck), so a box holding only a stray newline
      // removes the key everywhere alike.
      //
      // WHATEVER THE METHOD - encrypting or not - key text that carries
      // private-key material anywhere in it, of either kind, is refused, and
      // so is a paste over GatewayEncryption::MaximumKeyCharacters. A private
      // key must never be stored, backed up or reach the audit trail (which
      // withholds this column's value in any case: "encryptkey" is one of
      // AuditTrail's secret names).
      //
      // Then the key is judged as a key - by the one reader that judges it
      // again at every delivery (GatewayEncryption::Inspect): parseable, not
      // expired, and one a message can be encrypted to - when the save
      // CHANGES it: a new policy, a key that differs from the one the
      // database holds, or a method switched on or changed. A save that
      // changes neither does not judge it again, so that once a partner's key
      // expires the policy's other fields - the fallback above all, which
      // exists for exactly that day - can still be saved; the listing's
      // encrypt_key_problem says what is wrong with the key held. The record
      // compared with is read from the database afresh: over COM the object
      // being saved is the running record itself (InterfaceRemoteDomainPolicy
      // hands out the collection's own item), so the collection would compare
      // it with itself. A key given while the policy encrypts nothing is
      // judged as what it looks like (GatewayEncryption::MethodOfKey), so that
      // a key kept for later is one that will work; switching encryption OFF
      // judges nothing. A key that names no address in the partner's domain
      // is NOT refused (a gateway's key often names the gateway); the REST API
      // and the Control Panel show that as a warning beside the fingerprint.
      // A restore takes the key as the backup held it, above, so that a
      // backup of a key that has expired since still restores - and delivery
      // then refuses it by the policy's fallback.
      String key = policy->GetEncryptKey();
      key.Trim();
      policy->SetEncryptKey(key);

      if (!key.IsEmpty())
      {
         AnsiString keyText;
         Unicode::WideToMultiByte(key, keyText);

         AnsiString problem;
         if (!GatewayEncryption::AcceptableKeyText(keyText, problem))
         {
            String why = String(problem);
            why.TrimRight(_T('.'));

            resultDescription = String(_T("The partner's key was not saved: ")) + why + _T(".");
            return false;
         }
      }

      bool keyChanged = true;
      bool methodChanged = true;

      if (policy->GetID() > 0)
      {
         std::shared_ptr<RemoteDomainPolicy> stored = std::shared_ptr<RemoteDomainPolicy>(new RemoteDomainPolicy());
         if (PersistentRemoteDomainPolicy::ReadObject(stored, (long) policy->GetID()))
         {
            String storedKey = stored->GetEncryptKey();
            storedKey.Trim();

            keyChanged = storedKey != key;
            methodChanged = stored->GetEncrypt() != policy->GetEncrypt();
         }
      }

      const RemoteEncryption method = policy->GetEncrypt();
      const bool judge = method != RemoteEncryptNone ? (keyChanged || methodChanged) : (keyChanged && !key.IsEmpty());

      if (judge)
      {
         const RemoteEncryption judgedAs = method != RemoteEncryptNone ? method : GatewayEncryption::MethodOfKey(key);

         const GatewayEncryption::KeyFacts facts = GatewayEncryption::Inspect(judgedAs, key,
            policy->GetDomainName(), (__int64) time(nullptr));

         if (!facts.usable)
         {
            String why = String(facts.problem);
            why.TrimRight(_T('.'));

            resultDescription = method != RemoteEncryptNone
               ? String(_T("The partner's key cannot be used for ")) + String(GatewayEncryption::MethodName(method)) + _T(" encryption: ") + why + _T(".")
               : String(_T("The partner's key was not saved: it cannot be used for ")) + String(GatewayEncryption::MethodName(judgedAs)) + _T(" encryption: ") + why + _T(".");
            return false;
         }
      }

      return true;
   }

   bool
   PreSaveLimitationsCheck::CheckLimitations(PersistenceMode mode, std::shared_ptr<SecurityRange> securityRange, String &resultDescription)
   {
      if (mode == PersistenceModeRestore || mode == PersistenceModeRename)
         return true;

      std::shared_ptr<SecurityRange> existingRange = std::make_shared<SecurityRange>();
      if (PersistentSecurityRange::ReadObject(existingRange, securityRange->GetName()))
      {
         if (existingRange->GetID() != securityRange->GetID())
         {
            resultDescription = "There is already an IP range with this name.";
            return false;
         }
      }

      return true;
   }

}