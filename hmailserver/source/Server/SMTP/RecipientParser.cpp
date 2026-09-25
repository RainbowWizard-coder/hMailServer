// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "RecipientParser.h"

#include "SMTPConfiguration.h"

#include "PlusAddressing.h"

#include "../Common/Application/ObjectCache.h"
#include "../Common/Cache/CacheContainer.h"
#include "../Common/Util/QuotaPolicy.h"
#include "../Common/Util/MaskedAddresses.h"
#include "../Common/Util/DistributionListMembership.h"
#include "../Common/Util/ListManager.h"
#include "../Common/Util/InboundWebhooks.h"
#include "../Common/SQL/DatabaseUnavailableMarker.h"

#include "../Common/BO/Domain.h"
#include "../Common/BO/Alias.h"
#include "../Common/BO/Routes.h"
#include "../Common/BO/DistributionList.h"

#include "../Common/BO/DistributionListRecipients.h"
#include "../Common/BO/DistributionListRecipient.h"
#include "../Common/BO/MessageRecipient.h"
#include "../Common/BO/MessageRecipients.h"
#include "../Common/BO/RouteAddresses.h"
#include "../Common/BO/Account.h"
#include "../Common/BO/DomainAliases.h"

#include "../Common/Persistence/PersistentDistributionListRecipient.h"

#include "../Common/Util/SRS.h"
#include "../Common/Util/BATV.h"
#include "../Common/Application/IniFileSettings.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{

   const String CONST_UNKNOWN_USER = "Unknown user";

   RecipientParser::RecipientParser() :
      memo_added_count_(0)
   {

   }

   RecipientParser::~RecipientParser()
   {

   }

   RecipientParser::DeliveryPossibility
   RecipientParser::CheckDeliveryPossibility(bool bSenderIsAuthed, String sSender, const String &sOriginalRecipient, String &sErrMsg, bool &bTreatSecurityAsLocal, int iRecursionLevel, bool checkRecipientQuota)
   {
      bool bDomainIsLocal = false;
      bTreatSecurityAsLocal = false;

      // Apply domain name aliases to the sender address. Can be done
      // outside the loop since this won't be recursed.
      std::shared_ptr<DomainAliases> pDA = ObjectCache::Instance()->GetDomainAliases();
      sSender = pDA->ApplyAliasesOnAddress(sSender);

      String recipientAddress = sOriginalRecipient;

      // Set when the address, or one it led to, is routed to a URL by a route
      // that delivers to the mailbox as well (Util/InboundWebhooks.h): the
      // route makes the address one this server takes whether or not a mailbox
      // is found for it below.
      bool routedAlsoDelivered = false;

      // What an address that is not there - or is switched off - is answered
      // with: unknown, unless such a route has already made it one this server
      // takes, when the route alone receives it.
      auto unknownUnlessRouted = [&]() -> DeliveryPossibility
      {
         if (!routedAlsoDelivered)
            return DP_RecipientUnknown;

         sErrMsg.Empty();
         bDomainIsLocal = true;
         return DP_Possible;
      };

      while (true)
      {
         iRecursionLevel ++;

         if (iRecursionLevel > 25)
         {
            // Extreme aliasing. disallow
            sErrMsg = "Mail server configuration error. Too many recursive forwards.";
            return DP_RecipientUnknown;
         }

         // Apply domain name aliases on the recipient address.
         const String primaryAddressWithoutPlusaddressing = pDA->ApplyAliasesOnAddress(recipientAddress);
         const String primaryDomain = StringParser::ExtractDomain(primaryAddressWithoutPlusaddressing);
         
         std::shared_ptr<const Domain> pDomain = CacheContainer::Instance()->GetDomain(primaryDomain);
         bDomainIsLocal = pDomain != 0;

         // SRS reverse at RCPT time: a signed SRS0 bounce address at a local
         // domain is decoded to the original sender so the bounce can be relayed
         // back. The HMAC signature ensures only addresses this server generated
         // are reversible, so this does not create an open relay.
         if (pDomain && pDomain->GetIsActive() &&
             IniFileSettings::Instance()->GetSRSEnabled() &&
             SRS::IsSRS0(primaryAddressWithoutPlusaddressing))
         {
            String originalSender;
            if (SRS::Reverse(primaryAddressWithoutPlusaddressing, IniFileSettings::Instance()->GetSRSSecret(), originalSender))
            {
               // The valid SRS signature authorizes relaying this bounce back to
               // the original sender, so treat it like inbound local delivery
               // (no SMTP authentication / relay restriction is imposed).
               bTreatSecurityAsLocal = true;
               recipientAddress = originalSender;
               continue;
            }

            // An SRS0 address that fails signature validation is rejected.
            sErrMsg = CONST_UNKNOWN_USER;
            return DP_RecipientUnknown;
         }

         // BATV (prvs) reverse at RCPT time: a bounce addressed to a prvs-signed
         // local return-path is validated and stripped back to the original local
         // recipient. An invalid or expired signature is forged backscatter and is
         // rejected, which is the whole point of bounce-address tag validation.
         if (pDomain && pDomain->GetIsActive() &&
             IniFileSettings::Instance()->GetBATVEnabled() &&
             BATV::IsPrvs(primaryAddressWithoutPlusaddressing))
         {
            String originalRecipient;
            if (BATV::Verify(primaryAddressWithoutPlusaddressing, IniFileSettings::Instance()->GetBATVSecret(), originalRecipient))
            {
               // A valid prvs signature proves this is one of our own signed
               // return-paths, so authorize the (local) delivery without imposing
               // the SMTP authentication / relay restriction.
               bTreatSecurityAsLocal = true;
               recipientAddress = originalRecipient;
               continue;
            }

            // A prvs address that fails validation is rejected as backscatter.
            sErrMsg = CONST_UNKNOWN_USER;
            return DP_RecipientUnknown;
         }

         // Apply plus addressing on the recipient address
         const String primaryAddress = PlusAddressing::ExtractAccountAddress(primaryAddressWithoutPlusaddressing, pDomain); 

         // If this is a local domain, we should check for accounts, aliases and distribution lists.
         if (bDomainIsLocal)
         {
            if (iRecursionLevel == 1)
               {
               // Why only if iRecurse == 1?
               // Because:
               // If you have set up an alias pointing from user@local.com, user@external.com
               // you want to treat is as local even if the end account is remote.
               bTreatSecurityAsLocal = true;
            }

            if (!pDomain->GetIsActive())
            {
               sErrMsg = "Domain has been disabled.";
               return DP_RecipientUnknown;
            }

            // An address, or a whole domain, routed to a URL (InboundWebhooks.h).
            // Asked first, before the list manager's addresses, the accounts,
            // the aliases and the lists: the route is the administrator saying
            // where this address's mail goes, and a route that posts only takes
            // it whether or not a mailbox has the address - a list's posting
            // rules included, since the list never sees the message. One that
            // delivers as well goes on to the mailbox below, and so to a list's
            // posting rules, and is taken even when there is none. A route the
            // message has been posted to already (routes_taken_) is passed over.
            // One lock and an empty map on a server with no route.
            {
               InboundWebhooks::Route inboundRoute;
               if (InboundWebhooks::Instance()->FindRoute(primaryAddressWithoutPlusaddressing, inboundRoute) &&
                   routes_taken_.find(inboundRoute.id) == routes_taken_.end())
               {
                  if (!inboundRoute.deliver)
                  {
                     bDomainIsLocal = true;
                     return DP_Possible;
                  }

                  routedAlsoDelivered = true;
               }
            }

            // The addresses a managed list answers of its own (ListManager.h):
            // name-subscribe@, -unsubscribe@, -request@, -bounces@ with its VERP
            // form, and -owner@. Asked of the address as the RCPT gave it, before
            // plus addressing, so that a domain whose plus character is '-' does
            // not read name-request@ as name@ with a tag; an account, an alias or
            // a list of exactly that address is not one of them. An address that
            // ends in none of the five words costs a string test.
            {
               ListManager::Command listCommand = ListManager::CommandNone;
               std::shared_ptr<const DistributionList> managedList;
               String verpMember;

               if (ListManager::ParseCommandAddress(primaryAddressWithoutPlusaddressing, listCommand, managedList, verpMember))
               {
                  if (listCommand == ListManager::CommandOwner)
                  {
                     // The owner's address, followed as an alias is - so an owner
                     // that is itself a list's owner address ends at the
                     // recursion limit rather than in a loop.
                     const String owner = ListManager::OwnerOf(managedList);
                     if (owner.IsEmpty())
                     {
                        sErrMsg = CONST_UNKNOWN_USER;
                        return DP_RecipientUnknown;
                     }

                     recipientAddress = owner;
                     continue;
                  }

                  bDomainIsLocal = true;
                  return DP_Possible;
               }
            }

            // Check for an account with this name.
            std::shared_ptr<const Account> pAccount = CacheContainer::Instance()->GetAccount(primaryAddress);
            if (pAccount)
            {
               if (pAccount->GetActive())
               {
                  bDomainIsLocal = true;

                  // The mailbox exists and is active, so the only remaining question
                  // is whether anything will fit in it. Asked HERE, at RCPT time, and
                  // not left to delivery - because a message accepted for a full
                  // mailbox has to be answered with a non-delivery report, addressed
                  // to an envelope sender that spam has almost always forged. That
                  // makes this server the one sending mail to an innocent third
                  // party: backscatter, and a reputation problem earned on somebody
                  // else's behalf. Refusing during the SMTP conversation puts the
                  // failure back in front of the machine that actually connected.
                  if (checkRecipientQuota && IsMailboxFull_(pAccount, sErrMsg))
                     return DP_MailboxFull;

                  return DP_Possible;
               }
               else
               {
                  sErrMsg = "Account is not active.";
                  return unknownUnlessRouted();
               }
            }

            // If no account found, check if an alias with this name exists.
            std::shared_ptr<const Alias> pAlias = CacheContainer::Instance()->GetAlias(primaryAddress);
            if (pAlias)
            {
               if (pAlias->GetIsActive())
               {
                  recipientAddress = pAlias->GetValue();
                  continue;
               }
               else
               {
                  sErrMsg = "Alias is not active.";
                  return unknownUnlessRouted();
               }
            }

            // Check if distributionlist with this address exists.
            std::shared_ptr<const DistributionList> pList = CacheContainer::Instance()->GetDistributionList(primaryAddress);
            if (pList)
            {
               if (!pList->GetActive())
               {
                  sErrMsg = "Distribution list is not active.";
                  return unknownUnlessRouted();
               }

               String moderationKey = primaryAddress;
               moderationKey.ToLower();

               // Need to check if this sender is authorized to send
               // to this distribution list.
               if (UserCanSendToList_(sSender, bSenderIsAuthed, pList, sErrMsg, iRecursionLevel) == DP_PermissionDenied)
               {
                  if (pList->GetModeratorAddress().IsEmpty())
                     return DP_PermissionDenied;

                  // The list's mode refuses this sender, but the list has a
                  // moderator: the posting is accepted and, when this address is
                  // expanded a moment later, forwarded to the moderator for
                  // approval instead of being refused or distributed. The refusal
                  // reason is deliberately discarded - the sender is told 250, not
                  // why they were nearly refused - because a moderated list's whole
                  // point is that the sender cannot tell approval from delivery.
                  String msg;
                  msg.Format(_T("RecipientParser - Posting from '%s' to distribution list '%s' requires approval (%s) and will be forwarded to the moderator."),
                     sSender.c_str(), pList->GetAddress().c_str(), sErrMsg.c_str());
                  LOG_APPLICATION(msg);

                  sErrMsg.Empty();

                  moderated_list_addresses_.insert(moderationKey);

                  bDomainIsLocal = true;
                  return DP_Possible;
               }

               // The sender may post directly. Erase any moderation mark a refused
               // sender left for this list earlier on the same connection - the
               // mark must describe THIS message's decision, and a stale one would
               // send an authorized posting to the moderator instead of the list.
               moderated_list_addresses_.erase(moderationKey);

               bDomainIsLocal = true;
               return DP_Possible;
            }

            // A masked address a reader made (MaskedAddresses.h), after the
            // accounts, aliases and lists so that an administrator's address
            // always wins: delivered to its owner while it is on, and refused as
            // an unknown address once it is off - not handed to a catch-all.
            MaskedAddresses::Entry masked;
            if (MaskedAddresses::FindByAddress(primaryAddress, masked))
            {
               std::shared_ptr<const Account> owner = masked.active ? CacheContainer::Instance()->GetAccount(masked.account_id) : std::shared_ptr<const Account>();
               if (!owner)
               {
                  sErrMsg = CONST_UNKNOWN_USER;
                  return unknownUnlessRouted();
               }

               recipientAddress = owner->GetAddress();
               continue;
            }

            // OK, we are now finished looking through the domain.
         }

         // We have not found the recipient yet. Check if the original address matches a route.
         String recipientDomain = StringParser::ExtractDomain(recipientAddress);
         std::shared_ptr<Route> pRoute = Configuration::Instance()->GetSMTPConfiguration()->GetRoutes()->GetItemByNameWithWildcardMatch(recipientDomain);
         
         if (pRoute)
         {
            if (pRoute->ToAllAddresses() || pRoute->GetAddresses()->GetItemByName(recipientAddress))
            {
               if (iRecursionLevel == 1)
                  bTreatSecurityAsLocal = pRoute->GetTreatRecipientAsLocalDomain();

               return DP_Possible;
            }

            if (!pDomain || pDomain->GetPostmaster().IsEmpty())
            {
               // The recipient is not configured in the route, and the domain is external.
               sErrMsg = "Recipient not in route list.";
               return unknownUnlessRouted();
            }
         }

         // If this is a local domain, try to find a catch-all 
         // account for this domain.

         if (pDomain)
         {
            String sPostMaster = pDomain->GetPostmaster();

            if (!sPostMaster.IsEmpty())
            {
               // Could not find the address, but a post master was specified,
               // so we'll send to him instead.
               // Found an alias.
               recipientAddress = sPostMaster;
               continue;
            }
         }
         else
         {
            // Domain is not local. SMTPConnection should determine
            // whether the sender is allowed to send.
            return DP_Possible;
         }

         sErrMsg = CONST_UNKNOWN_USER;
         return unknownUnlessRouted();
      }
   }

   void 
   RecipientParser::CreateMessageRecipientList(const String &sRecipientAddress, std::shared_ptr<MessageRecipients> pRecipients, bool &recipientOK)
   {
      recipientOK = false;

      long lRecurse = 0;
      CreateMessageRecipientList_(sRecipientAddress, sRecipientAddress, 0, pRecipients, recipientOK);
   }

   void
   RecipientParser::CreateMessageRecipientList_(const String &recipientAddress, const String &sOriginalAddress, long lRecurse, std::shared_ptr<MessageRecipients> pRecipients, bool &recipientOK)
   {
      lRecurse++;

      if (lRecurse>25)
      {
         // To deep recursion! 
         return;
      }

      std::shared_ptr<DomainAliases> pDA = ObjectCache::Instance()->GetDomainAliases();
      String primaryAddress = pDA->ApplyAliasesOnAddress(recipientAddress);
      String primaryDomain = StringParser::ExtractDomain(primaryAddress);

      // First check if the domain is remote. If it is, we don't really
      // have to care what type of email this is.

      
      std::shared_ptr<const Domain> pDomain = CacheContainer::Instance()->GetDomain(primaryDomain);
      
      // SRS reverse: a signed SRS0 bounce address at a local domain is decoded
      // and delivered to the original (external) sender. This must run before
      // plus-addressing and account resolution because the encoded source
      // local-part may itself contain '+' or '=' characters.
      if (pDomain && pDomain->GetIsActive() &&
          IniFileSettings::Instance()->GetSRSEnabled() && SRS::IsSRS0(primaryAddress))
      {
         String originalSender;
         if (SRS::Reverse(primaryAddress, IniFileSettings::Instance()->GetSRSSecret(), originalSender))
            CreateMessageRecipientList_(originalSender, sOriginalAddress, lRecurse, pRecipients, recipientOK);

         // An SRS address is never a real local account; an invalid or expired
         // one is simply rejected (recipientOK stays false).
         return;
      }

      // BATV (prvs) reverse: a validated prvs return-path is stripped to the
      // original local recipient; an invalid one is dropped (forged backscatter).
      if (pDomain && pDomain->GetIsActive() &&
          IniFileSettings::Instance()->GetBATVEnabled() && BATV::IsPrvs(primaryAddress))
      {
         String originalRecipient;
         if (BATV::Verify(primaryAddress, IniFileSettings::Instance()->GetBATVSecret(), originalRecipient))
            CreateMessageRecipientList_(originalRecipient, sOriginalAddress, lRecurse, pRecipients, recipientOK);

         // A prvs address is never itself a real local account; an invalid one is
         // simply dropped (recipientOK stays false).
         return;
      }

      // The address before plus addressing, which is what the list manager's
      // own addresses are read from (see CheckDeliveryPossibility).
      const String addressBeforePlus = primaryAddress;

      // Apply plus addressing on the recipient address
      primaryAddress = PlusAddressing::ExtractAccountAddress(primaryAddress, pDomain);

      bool bIsLocalDomain = pDomain ? true : false;
      if (bIsLocalDomain)
      {
         // First check if this domain is really active.
         if (!pDomain->GetIsActive())
            return;

         // An address routed to a URL (InboundWebhooks.h): one recipient with no
         // account behind it, written as the RCPT gave it - a plus tag kept, for
         // the application it is posted to - which InboundWebhooks takes off the
         // message in the delivery path, before the list manager and local
         // delivery. A route that delivers as well puts back what the address
         // reaches without it, from a parser told which routes have the message
         // already. Before the list manager's addresses and the accounts, as
         // CheckDeliveryPossibility asks.
         {
            InboundWebhooks::Route inboundRoute;
            if (InboundWebhooks::Instance()->FindRoute(addressBeforePlus, inboundRoute) &&
                routes_taken_.find(inboundRoute.id) == routes_taken_.end())
            {
               std::shared_ptr<MessageRecipient> routedRecipient = std::shared_ptr<MessageRecipient>(new MessageRecipient);
               routedRecipient->SetAddress(addressBeforePlus);
               routedRecipient->SetOriginalAddress(sOriginalAddress);
               routedRecipient->SetLocalAccountID(0);
               routedRecipient->SetIsLocalName(true);

               recipientOK = true;

               AddRecipient_(pRecipients, routedRecipient);
               return;
            }
         }

         // An address a managed list answers of its own (ListManager.h): one
         // recipient with no account behind it, which the list manager answers
         // in the delivery path - or, for name-owner@, the owner, followed as an
         // alias is.
         {
            ListManager::Command listCommand = ListManager::CommandNone;
            std::shared_ptr<const DistributionList> managedList;
            String verpMember;

            if (ListManager::ParseCommandAddress(addressBeforePlus, listCommand, managedList, verpMember))
            {
               if (listCommand == ListManager::CommandOwner)
               {
                  const String owner = ListManager::OwnerOf(managedList);
                  if (!owner.IsEmpty())
                     CreateMessageRecipientList_(owner, sOriginalAddress, lRecurse, pRecipients, recipientOK);
                  return;
               }

               std::shared_ptr<MessageRecipient> commandRecipient = std::shared_ptr<MessageRecipient>(new MessageRecipient);
               commandRecipient->SetAddress(addressBeforePlus);
               commandRecipient->SetOriginalAddress(sOriginalAddress);
               commandRecipient->SetLocalAccountID(0);
               commandRecipient->SetIsLocalName(true);

               recipientOK = true;

               AddRecipient_(pRecipients, commandRecipient);
               return;
            }
         }

         // Check if there exists a account with this address.
         std::shared_ptr<const Account> pAccount = CacheContainer::Instance()->GetAccount(primaryAddress);

         if (pAccount)
         {
            if (!pAccount->GetActive())
               return;

            std::shared_ptr<MessageRecipient> NewRecipient = std::shared_ptr<MessageRecipient>(new MessageRecipient);
         
            NewRecipient->SetLocalAccountID(pAccount->GetID());
            NewRecipient->SetAddress(primaryAddress);
            NewRecipient->SetOriginalAddress(sOriginalAddress);
            NewRecipient->SetIsLocalName(true);

            recipientOK = true;

            AddRecipient_(pRecipients, NewRecipient);

            return;
         }
   
         // Check if there is a alias with this address.
         std::shared_ptr<const Alias> pAlias = CacheContainer::Instance()->GetAlias(primaryAddress);

         if (pAlias)
         {
            if (!pAlias->GetIsActive())
               return;

            CreateMessageRecipientList_(pAlias->GetValue(), sOriginalAddress, lRecurse, pRecipients, recipientOK);

            return;
         }  

         // Check if there is a distribution list with this address.
         std::shared_ptr<const DistributionList> pListADO = CacheContainer::Instance()->GetDistributionList(primaryAddress);

         if (pListADO)
         {
            if (!pListADO->GetActive())
               return;

            String moderationKey = primaryAddress;
            moderationKey.ToLower();

            if (moderated_list_addresses_.find(moderationKey) != moderated_list_addresses_.end())
            {
               // CheckDeliveryPossibility decided, a moment ago on this same
               // parser, that this sender's posting to this list needs approval.
               // The posting therefore expands to the MODERATOR and to nobody
               // else - not refused, not distributed. The original address is
               // passed through unchanged, exactly as member expansion does, so
               // the forward still identifies as a posting to the list, which is
               // what lets DistributionListSender stamp it.
               const String moderatorAddress = pListADO->GetModeratorAddress();

               if (moderatorAddress.IsEmpty())
               {
                  // The mark implies a moderator existed when the check ran, so
                  // getting here means the list was edited in the instants
                  // between. Refusing the recipient (recipientOK untouched) is
                  // the safe answer: expanding to members instead would
                  // distribute a message the check decided needed approval.
                  ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6360, "RecipientParser::CreateMessageRecipientList_",
                     Formatter::Format("Distribution list {0} needed a posting forwarded for approval, but no longer has a moderator address. The recipient was refused.",
                        pListADO->GetAddress()));
                  return;
               }

               if (moderatorAddress.CompareNoCase(pListADO->GetAddress().c_str()) == 0)
               {
                  // A list moderating itself would recurse straight back here, be
                  // found marked again, and chase its own tail to the recursion
                  // limit, reporting a failure at every level on the way back up.
                  // Refused once, with the reason, instead.
                  ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6363, "RecipientParser::CreateMessageRecipientList_",
                     Formatter::Format("Distribution list {0} has its own address configured as its moderator address, so a posting needing approval has nowhere to go. The recipient was refused.",
                        pListADO->GetAddress()));
                  return;
               }

               // owned by this call: the recipient list this parser is building, read again after the recursion below appends to it
               std::vector<std::shared_ptr<MessageRecipient> > &addedRecipients = pRecipients->GetVector();
               const size_t countBeforeModerator = addedRecipients.size();

               bool moderatorOK = false;
               CreateMessageRecipientList_(moderatorAddress, sOriginalAddress, lRecurse, pRecipients, moderatorOK);

               if (!moderatorOK)
               {
                  // The moderator address does not resolve to anything mail can
                  // reach. The RCPT is refused (recipientOK untouched), which the
                  // sender sees as 550 - a posting silently lost between the list
                  // and a broken moderator address would be worse, and would look
                  // exactly like moderation working.
                  ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6361, "RecipientParser::CreateMessageRecipientList_",
                     Formatter::Format("Distribution list {0} needed a posting forwarded for approval, but its moderator address {1} does not resolve to any deliverable recipient. The recipient was refused.",
                        pListADO->GetAddress(), moderatorAddress));
                  return;
               }

               // Everything the recursion just appended is the moderation
               // forward. Marked per recipient rather than per message so the
               // marker travels with exactly the copies it describes.
               for (size_t i = countBeforeModerator; i < addedRecipients.size(); i++)
                  addedRecipients[i]->SetIsModerationForward(true);

               recipientOK = true;

               return;
            }

            // A managed list (ListManager.h) is one recipient of its own rather
            // than its members: it sends each member a copy of their own from
            // the delivery path, with that member's way out in its headers. Who
            // may post was decided above, and a posting the mode refused has
            // gone to the moderator, exactly as for any list.
            if (pListADO->GetManaged())
            {
               std::shared_ptr<MessageRecipient> listRecipient = std::shared_ptr<MessageRecipient>(new MessageRecipient);
               listRecipient->SetAddress(pListADO->GetAddress());
               listRecipient->SetOriginalAddress(sOriginalAddress);
               listRecipient->SetLocalAccountID(0);
               listRecipient->SetIsLocalName(true);

               recipientOK = true;

               AddRecipient_(pRecipients, listRecipient);
               return;
            }

            // The list's rows, and then what its membership query answers now
            // (DistributionListMembership.h): the accounts of a domain, those
            // matching a pattern, or everyone, evaluated for this message and
            // never stored. Each goes through the same expansion a row does, so
            // an account's plus address, its active flag and its domain's are
            // judged the one way, and AddRecipient_ keeps an address that is
            // both a row and an answer to one copy.
            bool membershipUnavailable = false;
            const std::vector<String> members = MembersOf_(pListADO, pRecipients, membershipUnavailable);

            if (membershipUnavailable)
            {
               // The database did not answer, so who this list delivers to is
               // not known. Nothing is expanded and recipientOK is left alone,
               // which is what makes this a refusal rather than a delivery:
               // with the thread marked, the scope SMTPConnection opened around
               // this call answers 451 and the sender retries.
               //
               // Marked here rather than left to whichever read failed, because
               // this is the layer that knows the answer is missing rather than
               // empty: the pool marks a statement it could not run, but a
               // membership can also fail to be answered without one (a domain
               // the cache could not read), and a 550 for any of them would
               // bounce mail for a list that is perfectly good.
               //
               // Expanding the rows here instead is the defect this exists to
               // stop. One row would set recipientOK, the 451 would never fire,
               // and a message for a thousand query members would be accepted
               // with 250 and delivered to the one row - a loss nothing ever
               // retries, because the sender was told it arrived.
               // DistributionListMembership::Members has already reported 6982.
               DatabaseUnavailableMarker::Mark();

               return;
            }

            for (const String &member : members)
               CreateMessageRecipientList_(member, sOriginalAddress, lRecurse, pRecipients, recipientOK);

            return;
         }

         // A masked address: its owner, counted, while it is on.
         MaskedAddresses::Entry masked;
         if (MaskedAddresses::FindByAddress(primaryAddress, masked))
         {
            std::shared_ptr<const Account> owner = masked.active ? CacheContainer::Instance()->GetAccount(masked.account_id) : std::shared_ptr<const Account>();
            if (!owner)
               return;

            MaskedAddresses::CountDelivery(masked.id);
            CreateMessageRecipientList_(owner->GetAddress(), sOriginalAddress, lRecurse, pRecipients, recipientOK);
            return;
         }
      }
      
      // Check for routes. This happens under two circumstances:
      // 1) The domain is local but the recipient user was not found in the domain.
      // 2) The domain is not local. 
      // When we get here, one of these are true.
      String recipientDomain = StringParser::ExtractDomain(recipientAddress);
      std::shared_ptr<Route> pRoute = HM::Configuration::Instance()->GetSMTPConfiguration()->GetRoutes()->GetItemByNameWithWildcardMatch(recipientDomain);
      if (pRoute)
      {
         bool isLocalName = false;

         if (pRoute->ToAllAddresses() || pRoute->GetAddresses()->GetItemByName(recipientAddress))
         {
            isLocalName = pRoute->GetTreatRecipientAsLocalDomain();

            std::shared_ptr<MessageRecipient> NewRecipient = std::shared_ptr<MessageRecipient>(new MessageRecipient);
            NewRecipient->SetAddress(recipientAddress);
            NewRecipient->SetOriginalAddress(sOriginalAddress);
            NewRecipient->SetIsLocalName(isLocalName);

            recipientOK = true;

            AddRecipient_(pRecipients, NewRecipient);
            return;
         }

         if (!bIsLocalDomain || pDomain->GetPostmaster().IsEmpty())
            return;
      }

      if (bIsLocalDomain)
      {
         String sPostMaster = pDomain->GetPostmaster();
         if (!sPostMaster.IsEmpty())
         {
            // The domain is local but we could not find the recipient address
            // in our domain. We've looked in routes as well but not found a
            // match there either.
            CreateMessageRecipientList_(sPostMaster, sOriginalAddress, lRecurse, pRecipients, recipientOK);

            return;
         }
      }
      else
      {
         // The recipient is external. We have already checked if it's OK that the user
         // delivers to this, so go ahead.

         std::shared_ptr<MessageRecipient> NewRecipient = std::shared_ptr<MessageRecipient>(new MessageRecipient);

         NewRecipient->SetAddress(recipientAddress);
         NewRecipient->SetOriginalAddress(sOriginalAddress);
         NewRecipient->SetLocalAccountID(0);
         NewRecipient->SetIsLocalName(false);

         recipientOK = true;

         AddRecipient_(pRecipients, NewRecipient);
      }
   }

   bool
   RecipientParser::IsMailboxFull_(std::shared_ptr<const Account> account, String &sErrMsg)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Whether the account is ALREADY at or over its quota - not whether the message
   // about to arrive would overflow it.
   //
   // That distinction is deliberate. The SIZE parameter (RFC 1870) is optional, so a
   // check that depended on it would work only for senders that volunteer one, and
   // would be absent for exactly the traffic least likely to declare anything. And
   // the case that produces backscatter is not the message that tips a mailbox over
   // the edge - it is the mailbox that has been sitting full for a week while several
   // hundred messages arrive and are each answered with a bounce. That case is
   // answerable without knowing any size at all.
   //
   // The marginal message that would overflow a mailbox with room left is still
   // handled where it always was, in LocalDelivery::CheckAccountQuotas_, so nothing
   // that used to be caught stops being caught.
   //---------------------------------------------------------------------------()
   {
      if (!IniFileSettings::Instance()->GetRejectFullMailboxAtRcpt())
         return false;

      // Unlimited.
      if (account->GetAccountMaxSize() == 0)
         return false;

      // One byte, not zero: SpaceAvailable(0) answers "is the mailbox at most exactly
      // full", which would let a mailbox sitting precisely on its limit go on
      // accepting mail that cannot be stored.
      if (QuotaPolicy::AcceptsDelivery(account, 1))
         return false;

      sErrMsg = "Mailbox is full.";

      return true;
   }

   RecipientParser::DeliveryPossibility 
   RecipientParser::UserCanSendToList_(const String &sSender, bool bSenderIsAuthenticated, std::shared_ptr<const DistributionList> pList, String &sErrMsg, int iRecursionLevel)
   {
      std::shared_ptr<DomainAliases> pDA = ObjectCache::Instance()->GetDomainAliases();

      // The moderator of a list may always post to it - this is what approval IS.
      // A moderated posting is forwarded to the moderator, and the moderator
      // approves it by resending it to the list; this bypass is what lets that
      // resend through the very mode checks that sent the original to moderation.
      //
      // The authentication requirement is unconditional and is the security
      // boundary, deliberately NOT the list's own RequireAuth flag: MAIL FROM is
      // free text, so an unauthenticated session claiming the moderator's address
      // proves nothing and falls through to the ordinary mode checks below - where
      // a mode that refuses it sends the forgery to the real moderator instead of
      // the members. What authentication proves here is possession of valid
      // credentials on this server, which is the same trust the existing
      // RequireAuth machinery extends to an announcement list's owner address.
      const String moderatorAddress = pList->GetModeratorAddress();

      const bool isAuthenticatedModerator =
         !moderatorAddress.IsEmpty() &&
         bSenderIsAuthenticated &&
         pDA->ApplyAliasesOnAddress(sSender).CompareNoCase(pDA->ApplyAliasesOnAddress(moderatorAddress)) == 0;

      if (isAuthenticatedModerator)
      {
         Logger::Instance()->LogDebug("DistributionList::AuthenticatedModerator");

         // Skip the RequireAuth and mode checks - all of them are what the
         // moderator moderates - but fall through to the recursive check below:
         // whether the moderator may reach every recipient of the list is a
         // question about the RECIPIENTS, and being moderator answers nothing
         // about it.
      }
      else
      {

      if (pList->GetRequireAuth() && !bSenderIsAuthenticated)
      {
         sErrMsg = "SMTP authentication required.";
         return DP_PermissionDenied;
      }

      DistributionList::ListMode lm = pList->GetListMode();

      if (lm == DistributionList::LMAnnouncement)
      {
         // Only one person can send to list. Check if it's the correct. Before we do the
         // comparision, we resolve any domain name aliases.

         Logger::Instance()->LogDebug("DistributionList::LMAnnouncement");

         String sFormattedSender = pDA->ApplyAliasesOnAddress(sSender);
         String sFormattedRequiredSender = pDA->ApplyAliasesOnAddress(pList->GetRequireAddress());
         if (sFormattedSender.CompareNoCase(sFormattedRequiredSender) != 0)
         {
       // Let's adjust reason to better explain sender is not seen as OWNER
       // and differentiate from SENDER like list member etc
            sErrMsg = "Not authorized owner.";
            return DP_PermissionDenied;
         }

         // OK. The correct user is sending
      }
      else if (lm == DistributionList::LMDomainMembers)
      {
         Logger::Instance()->LogDebug("DistributionList::LMDomainMembers");

         String sFormattedSender = pDA->ApplyAliasesOnAddress(sSender);
         String sFormattedDomain = StringParser::ExtractDomain(sFormattedSender);

         __int64 iDomainID = pList->GetDomainID();
         std::shared_ptr<const Domain> pDomain = CacheContainer::Instance()->GetDomain(iDomainID);

         if (sFormattedDomain.CompareNoCase(pDomain->GetName()) != 0)
         {
            // Let's adjust reason to better explain sender is not seen as OWNER
            // and differentiate from SENDER like list member etc
            sErrMsg = "Not authorized domain.";
            return DP_PermissionDenied;
         }

         // OK. The correct domain is sending
      }
      else if (lm == DistributionList::LMPublic)
      {
         // Anyone can send. OK
         Logger::Instance()->LogDebug("DistributionList::LMPublic");
      }
      else if (lm == DistributionList::LMMembership)
      {
         // Only members of the list can send messages. 
         // Check if the sender is a member of the list.
         // owned by this call: DistributionList::GetMembers() loads a new collection for every caller
         std::vector<std::shared_ptr<DistributionListRecipient> > vecRecipients = pList->GetMembers()->GetVector();
         auto iterRecipient = vecRecipients.begin();

         Logger::Instance()->LogDebug("DistributionList::LMMembership");

         for (; iterRecipient != vecRecipients.end(); iterRecipient++)
         {
            // A request to join a managed list, still waiting for its own
            // confirmation or for the owner, is not a member and may not post.
            if (!(*iterRecipient)->GetIsMember())
               continue;

            String sRecipient = (*iterRecipient)->GetAddress();
            sRecipient = pDA->ApplyAliasesOnAddress(sRecipient);

            if (sRecipient.CompareNoCase(sSender) == 0)
            {
               break;
            }

         }

         // If we reached the end of the list, it means that we
         // didn't find the recipient among the rows. A list with a membership
         // query has members its rows do not name - the accounts the query
         // answers - and a sender who is one of them is a member. Asked of the
         // cache by IsMember rather than by resolving the whole query, since
         // this runs at RCPT TO; the sender's domain alias is applied first so
         // that the account lookup sees the primary address.
         if (iterRecipient == vecRecipients.end() &&
             !DistributionListMembership::IsMember(pList, pDA->ApplyAliasesOnAddress(sSender)))
         {
            // Let's adjust reason to better explain sender is not seen as allowed SENDER
            sErrMsg = "Not authorized sender.";
            return DP_PermissionDenied;
         }
      }

      } // end of the non-moderator path; the recursive check below runs for both.


      // Check that the user is allowed to send to all recipient
      // of the list. This is a bit CPU intensive, but we need
      // to recursively look up all the recipients.
      //
      // The rows only, not the membership query's answers: this walk exists
      // for a row that is an address elsewhere (may this sender relay to it)
      // or another list (does that list take this sender), and a query answers
      // active accounts of this server and nothing else, for which the answer
      // is yes without asking. Resolving a domain's accounts at every RCPT TO
      // to ask a question with one answer would be the cost without the check.
      std::shared_ptr<DistributionListRecipients> pListMembers = pList->GetMembers();

      // owned by this call: DistributionList::GetMembers() loads a new collection for every caller
      std::vector<std::shared_ptr<DistributionListRecipient> > vecRecipients = pListMembers->GetVector();
      std::vector<std::shared_ptr<DistributionListRecipient> >::const_iterator iterRecipient = vecRecipients.begin();

      while (iterRecipient != vecRecipients.end())
      {
         bool bTreatSecurityAsLocal = true;

         DeliveryPossibility dp = CheckDeliveryPossibility(bSenderIsAuthenticated, sSender, (*iterRecipient)->GetAddress(), sErrMsg, bTreatSecurityAsLocal, iRecursionLevel);
         if (dp == DP_PermissionDenied)
         {
            // Log the reason the message to the list is rejected which helps a ton with lists on lists
            Logger::Instance()->LogDebug("RecipientParser::UserCanSendToList_::PermissionDENIED");

            return DP_PermissionDenied;
         }
         iterRecipient++;
      }

      return DP_Possible;
   }

   void
   RecipientParser::AddRecipient_(std::shared_ptr<MessageRecipients> pRecipients, std::shared_ptr<MessageRecipient> pRecipient)
   {
      String address = pRecipient->GetAddress().ToLower();

      if (address.IsEmpty())
         return;

      ForgetIfNewTransaction_(pRecipients);

      // owned by this call: the recipient list this parser is building
      const std::vector<std::shared_ptr<MessageRecipient> > &vecResult = pRecipients->GetVector();

      if (vecResult.size() != memo_added_count_)
      {
         // The collection is not what this parser last left it as - a caller
         // that emptied it, a recursion that filled it before this parser saw
         // it - so the index is built from what is actually in it rather than
         // trusted. This is the same answer the walk it replaces would give,
         // asked once instead of once per insert.
         memo_added_.clear();

         for (const std::shared_ptr<MessageRecipient> &existing : vecResult)
         {
            String lowered = existing->GetAddress();
            lowered.ToLower();
            memo_added_.insert(lowered);
         }

         memo_added_count_ = vecResult.size();
      }

      if (!memo_added_.insert(address).second)
         return;

      pRecipients->Add(pRecipient);
      // owned by this call: the recipient list this parser is building
      memo_added_count_ = pRecipients->GetVector().size();
   }

   std::vector<String>
   RecipientParser::MembersOf_(std::shared_ptr<const DistributionList> list, std::shared_ptr<MessageRecipients> pRecipients, bool &unavailable)
   {
      unavailable = false;

      ForgetIfNewTransaction_(pRecipients);

      String key = list->GetAddress();
      key.ToLower();

      std::map<String, std::vector<String> >::const_iterator remembered = membership_memo_.find(key);

      if (remembered != membership_memo_.end())
         return remembered->second;

      const std::vector<String> members = DistributionListMembership::Members(list, unavailable);

      if (unavailable)
      {
         // Not remembered: the next command in this transaction should be free
         // to ask again, because the database may have come back by then, and
         // remembering "could not be answered" would turn one locked table into
         // a refusal for the rest of the transaction.
         return std::vector<String>();
      }

      membership_memo_[key] = members;

      return members;
   }

   void
   RecipientParser::ForgetIfNewTransaction_(std::shared_ptr<MessageRecipients> pRecipients)
   {
      if (memo_recipients_.lock() == pRecipients)
         return;

      memo_recipients_ = pRecipients;
      membership_memo_.clear();
      memo_added_.clear();
      memo_added_count_ = 0;
   }
}
