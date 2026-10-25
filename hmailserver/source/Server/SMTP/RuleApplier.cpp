// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"
#include "../Common/Util/RestrictedDelivery.h"

#include "./RuleApplier.h"
#include "../Common/Util/DisclaimerAdder.h"
#include "../Common/Util/ContentPolicy.h"
#include "../Common/BO/Domain.h"
#include "../Common/BO/Attachments.h"
#include "../Common/BO/Attachment.h"

#include "RuleResult.h"

#include "../Common/BO/Rule.h"
#include "../Common/BO/Rules.h"
#include "../Common/BO/RuleCriterias.h"
#include "../Common/BO/RuleActions.h"
#include "../Common/BO/RuleAction.h"
#include "../Common/BO/MessageData.h"
#include "../Common/BO/Message.h"
#include "../Common/Mime/Mime.h"
#include "../Common/BO/Account.h"
#include "../Common/BO/MessageRecipients.h"
#include "../Common/Cache/CacheContainer.h"
#include "../Common/Application/ObjectCache.h"
#include "../Common/Util/Time.h"
#include "../Common/Util/RegularExpression.h"
#include "../Common/Rules/RuleGuard.h"
#include "../Common/Util/SealedMessage.h"
#include "../Common/AntiSpam/DKIM/DKIMSigner.h"

#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Persistence/MessageStore.h"

#include "RecipientParser.h"
#include "DeliveryQueue.h"
#include "SMTPForwarding.h"
#include "../Common/Util/Utilities.h"

#include "SMTPConfiguration.h"

#include "../Common/Scripting/ScriptServer.h"
#include "../Common/Scripting/ScriptObjectContainer.h"
#include "../Common/Scripting/Result.h"

#include "../Common/BO/MessageRecipient.h"


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A rule's name and a reject's text go into one line of a 550 reply and
      // into the Subject and X-hMailServer-Policy headers of the notice a
      // policy sends. REST refuses a control character in either
      // (RestApiRules.cpp); the COM and Control Panel paths check nothing, so
      // the line break is taken out where the value is written, and whoever
      // could put one in cannot split the reply or add a header with it.
      String OneLine_(const String &text)
      {
         String line = text;
         line.Replace(_T("\r"), _T(" "));
         line.Replace(_T("\n"), _T(" "));
         return line;
      }

      // A message a rule queues - a forward, a copy, a reply, a notice to the
      // sender - is written from inside a delivery. The delivery manager reads
      // the queue again only when it is told to or once a minute, and it last
      // read it when it handed out the message being delivered, before this
      // one existed: untold, the new message waited up to that minute. Telling
      // it delivers nothing itself - it wakes the manager, which reads the
      // queue as it always does, and a drain's pause still holds. The
      // delivery-failure report (SMTPDeliverer::SubmitErrorLog_) and the
      // out-of-office reply tell it too. This is the Sieve redirect's way,
      // which, when the servers are stopping and the manager is already gone,
      // tells nobody and leaves the message for the next start, and not
      // Application::SubmitPendingEmail's, which reports a High error then.
      void DeliverQueuedMessageNow_()
      {
         DeliveryQueue::StartDelivery();
      }
   }

   RuleApplier::RuleApplier(void) :
      rule_account_id_(0),
      rule_domain_id_(0),
      recipient_scope_domain_(0)
   {
   }

   RuleApplier::~RuleApplier(void)
   {
   }

   void
   RuleApplier::ApplyRules(std::shared_ptr<Rules> pRules, std::shared_ptr<const Account> account, std::shared_ptr<Message> pMessage, RuleResult &ruleResult)
   {
      LOG_DEBUG(_T("Applying rules"));

      // One locked copy, taken once, and every question below asked of it. The
      // collection is ObjectCache's - an account's, a domain's or the global
      // rules - and an account's or a domain's is refreshed IN PLACE by
      // GetAccountRules/GetDomainRules on whichever thread asks first after a
      // rule is saved, the global ones by a restore. An index walk over the
      // live collection is memory-safe, since GetCount and GetItem each lock,
      // but a refresh between two GetItem calls re-indexes it under the walk:
      // a rule is skipped, so a delete or a move is missed for this message,
      // or one runs twice, so a forward or a copy is sent twice.
      const std::vector<std::shared_ptr<Rule> > rules = pRules->GetSnapshot();

      if (rules.empty())
      {
         return;
      }

      // A message too large to be read whole (MessageData::MaxParseBytes) is read
      // header only, so that the rules' header criteria and header actions apply
      // to it and its body criteria decline (MessageMatchesCriteria_). Until the
      // large-message row its load failed, the guard below reported HM6040, and
      // no rule ran on it at all.
      std::shared_ptr<MessageData> pMessageData = std::shared_ptr<MessageData>(new MessageData());
      const bool loaded = pMessageData->LoadFromMessage(account, pMessage, MessageData::LargeMessage::HeaderOnly);

      // The rule set must not run against a MessageData the engine could not populate.
      // Every criterion would then be compared against an empty string, NotEquals and
      // NotContains would match, and a "Subject NotContains [approved] -> Delete" rule
      // would destroy the message. RuleGuard reports it; we deliver unfiltered, because
      // unfiltered mail in an inbox is recoverable and deleted mail is not.
      if (!RuleGuard::MessageIsUsableForRules(loaded, account, pMessage))
         return;


      rule_account_id_ = pRules->GetAccountID();
      rule_domain_id_ = pRules->GetDomainID();

      for (std::shared_ptr<Rule> pRule : rules)
      {
         if (pRule && pRule->GetActive())
         {
            bool bContinueRuleProcessing = true;
            if (ApplyRule_(pRule, account, pMessageData, bContinueRuleProcessing, ruleResult))
            {
               // The rule matched. We should return here.
               return;
            }

            if (!bContinueRuleProcessing)
            {
               break;
            }
         }
      }
   }

   bool
   RuleApplier::ApplyRule_(std::shared_ptr<Rule> pRule, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData, bool &bContinueRuleProcessing, RuleResult &ruleResult)
   {
      if (Logger::Instance()->GetLogDebug())
         LOG_DEBUG(_T("Applying rule " + pRule->GetName()));

      bool bAllRequired = pRule->GetUseAND();
      bool bDoActions = false;

      // One line per rule per message, however many body criteria the rule has.
      // An administrator whose rule has stopped firing on encrypted mail needs
      // to be able to find out why; an administrator with thirty rules does not
      // need thirty lines about the same message.
      bool sealedBodyLogged = false;
      bool unreadBodyLogged = false;

      std::shared_ptr<RuleCriterias> pCriterias = pRule->GetCriterias();
      for (int i = 0; i < pCriterias->GetCount(); i++)
      {
         std::shared_ptr<RuleCriteria> pCriteria = pCriterias->GetItem(i);

         if (!pCriteria)
            continue;

         bool sealedBodyDeclined = false;
         bool unreadBodyDeclined = false;
         bool bMatch = MessageMatchesCriteria_(pCriteria, pMsgData, sealedBodyDeclined, unreadBodyDeclined);

         if (unreadBodyDeclined && !unreadBodyLogged)
         {
            unreadBodyLogged = true;

            std::shared_ptr<Message> ruleMessage = pMsgData->GetMessage();

            LOG_APPLICATION(Formatter::Format("RuleApplier - Message {0} is larger than the {1} MB a message is read whole at, so only its header was read: the criterion of rule \"{2}\" that needs its body or its attachments could not be evaluated and did not match. The rule's other criteria were evaluated.",
               ruleMessage ? ruleMessage->GetID() : 0,
               (int) (MessageData::MaxParseBytes / (1024 * 1024)),
               pRule->GetName()));
         }

         if (sealedBodyDeclined && !sealedBodyLogged)
         {
            sealedBodyLogged = true;

            std::shared_ptr<Message> ruleMessage = pMsgData->GetMessage();

            LOG_APPLICATION(Formatter::Format("RuleApplier - Message {0} is encrypted ({1}), so the body criterion of rule \"{2}\" could not be evaluated and did not match. The server cannot read the body of this message; it is unreadable rather than empty, and a rule that tested it for the absence of a word would otherwise have fired on every encrypted message.",
               ruleMessage ? ruleMessage->GetID() : 0,
               pMsgData->GetFieldValue(_T("Content-Type")),
               pRule->GetName()));
         }

         if (bAllRequired)
         {
            if (!bMatch)
            {
               return false;
            }

            bDoActions = true;
         }
         else
         {
            if (bMatch)
            {
               bDoActions = true;
               break;
            }
         }
      }

      if (bDoActions)
      {
         ApplyActions_(pRule, account, pMsgData, bContinueRuleProcessing, ruleResult);
      }

      return false;
   }

   void
   RuleApplier::ApplyActions_(std::shared_ptr<Rule> pRule, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData, bool &bContinueRuleProcessing, RuleResult &ruleResult)
   {  
      std::shared_ptr<RuleActions> pActions = pRule->GetActions();

      for (int i = 0; i < pActions->GetCount(); i++)
      {
         std::shared_ptr<RuleAction> pAction = pActions->GetItem(i);

         if (pAction)
         {
            ApplyAction_(pRule, pAction, account, pMsgData, bContinueRuleProcessing, ruleResult);
         }
      }
   }

   bool
   RuleApplier::OutsideScope_(const String &action) const
   {
      if (recipient_scope_domain_ == 0)
         return false;

      LOG_DEBUG("RuleApplier: the domain rule's " + action + " is not applied: the message also goes to recipients outside the domain, and it would change the message for them.");
      return true;
   }

   bool
   RuleApplier::SenderDomainVouched_(std::shared_ptr<Message> message, __int64 senderDomainId, const String &senderDomainName)
   {
      if (!message || senderDomainId <= 0)
         return false;

      // The account an authenticated session, the webmail or a scheduled send
      // submitted it as, when that account is the domain's.
      if (message->GetSubmitterAccountID() > 0)
      {
         std::shared_ptr<const Account> submitter = CacheContainer::Instance()->GetAccount(message->GetSubmitterAccountID());
         if (submitter && submitter->GetDomainID() == senderDomainId)
            return true;
      }

      // Or, for mail from outside, DMARC passed for the domain
      // (AuthenticationResults::PassedDomains: whole tokens, "dmarc=name").
      const String passed = message->GetAuthDomains();
      if (!passed.IsEmpty() && !senderDomainName.IsEmpty())
      {
         std::vector<String> tokens = StringParser::SplitString(passed, _T(","));
         for (String token : tokens)
         {
            token.Trim();
            if (token.CompareNoCase(_T("dmarc=") + senderDomainName) == 0)
               return true;
         }
      }

      return false;
   }

   std::vector<RuleApplier::DomainRuleScope>
   RuleApplier::DomainRuleScopes(std::shared_ptr<Message> message)
   {
      std::vector<DomainRuleScope> scopes;
      if (!message)
         return scopes;

      // Who the message is addressed to: each hosted domain with how many of
      // its recipients it has, in the order first met, and how many there
      // are in all.
      std::vector<std::pair<__int64, int> > recipientDomains;
      int total = 0;
      // owned by this call: the recipients of the message its caller holds
      for (std::shared_ptr<MessageRecipient> recipient : message->GetRecipients()->GetVector())
      {
         if (!recipient)
            continue;

         total++;

         if (!recipient->GetIsLocalName())
            continue;

         std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(StringParser::ExtractDomain(recipient->GetAddress()));
         if (!domain)
            continue;

         bool known = false;
         for (std::pair<__int64, int> &entry : recipientDomains)
         {
            if (entry.first == domain->GetID())
            {
               entry.second++;
               known = true;
               break;
            }
         }
         if (!known)
            recipientDomains.push_back(std::make_pair(domain->GetID(), 1));
      }

      std::set<__int64> sending;

      // The submitter's own domain: its account signed in and sent this.
      if (message->GetSubmitterAccountID() > 0)
      {
         std::shared_ptr<const Account> submitter = CacheContainer::Instance()->GetAccount(message->GetSubmitterAccountID());
         if (submitter && submitter->GetDomainID() > 0)
            sending.insert(submitter->GetDomainID());
      }

      // The envelope sender's hosted domain, when what the server saw vouches
      // for it, or when the message reaches no other domain's mailboxes - an
      // unauthenticated claim then selects rules that act on that domain's
      // own recipients and on mail leaving the server, which is what an
      // application relaying from its own network sends.
      std::shared_ptr<const Domain> senderDomain = CacheContainer::Instance()->GetDomain(StringParser::ExtractDomain(message->GetFromAddress()));
      if (senderDomain)
      {
         bool otherMailboxes = false;
         for (const std::pair<__int64, int> &entry : recipientDomains)
         {
            if (entry.first != senderDomain->GetID())
               otherMailboxes = true;
         }

         if (!otherMailboxes || SenderDomainVouched_(message, senderDomain->GetID(), senderDomain->GetName()))
            sending.insert(senderDomain->GetID());
      }

      for (__int64 domainId : sending)
      {
         DomainRuleScope scope;
         scope.domain_id = domainId;
         scope.whole_message = true;
         scopes.push_back(scope);
      }

      for (const std::pair<__int64, int> &entry : recipientDomains)
      {
         if (sending.find(entry.first) != sending.end())
            continue;

         DomainRuleScope scope;
         scope.domain_id = entry.first;
         scope.whole_message = entry.second == total;
         scopes.push_back(scope);
      }

      return scopes;
   }

   bool
   RuleApplier::RepeatsAttempt_(std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData, const String &action)
   {
      // An account's own rules are never repeated by a held pass: they run in
      // local delivery, once, when the recipient's copy is stored, and a
      // recipient delivered is taken off the message. The mark travels on the
      // message, which the recipient's copy copies the flags of (or which is
      // itself the copy, for a single recipient), so a recipient first delivered
      // on a held pass - one whose mailbox's encryption was not known on the
      // pass that ran the rules (LocalDelivery::HoldForRetry) - would otherwise
      // have its own forward, copy or reply skipped although it never ran.
      if (account)
         return false;

      // A pass that repeats the attempt the last pass made (Message::
      // FlagRepeatsAttempt): that pass ran these rules and was then held by this
      // server's own shaping without a try counted. The actions that only change
      // this message run again, as they must - a route, a header, a delete - but
      // one that queues a NEW message has already queued it, and doing so again
      // on every held pass sent one forward, copy or reply per pass: dozens for
      // one message held behind a rate or a back-off. A counted retry clears the
      // mark, and runs them as every counted retry always has.
      std::shared_ptr<Message> message = pMsgData ? pMsgData->GetMessage() : std::shared_ptr<Message>();
      if (!message || !message->GetRepeatsAttempt())
         return false;

      LOG_DEBUG("RuleApplier: the rule's " + action + " is not made again: this pass repeats a delivery attempt that already made it.");
      return true;
   }

   void
   RuleApplier::ApplyAction_(std::shared_ptr<Rule> pRule, std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData, bool &bContinueRuleProcessing, RuleResult &ruleResult)
   {  
      Logger::Instance()->LogDebug(_T("Performing rule action"));
      switch (pAction->GetType())
      {
      case RuleAction::Delete:  
         {
            ruleResult.SetDeleteEmail(true);

            String sDeleteRuleName;
            sDeleteRuleName.Format(_T("Rule name: %s, ID: %d"), pRule->GetName().c_str(), pRule->GetID());

            ruleResult.SetDeleteRuleName(sDeleteRuleName);
            break;
         }
      case RuleAction::MoveToIMAPFolder:
         {
            ruleResult.SetMoveToFolder(pAction->GetIMAPFolder());
            break;
         }
      case RuleAction::Forward:  
         {
            if (RepeatsAttempt_(account, pMsgData, _T("forward")))
               break;

            ApplyAction_Forward(pAction, account, pMsgData);
            break;
         }
      case RuleAction::Reply:
         {
            if (RepeatsAttempt_(account, pMsgData, _T("reply")))
               break;

            ApplyAction_Reply(pRule, pAction, account, pMsgData);
            break;
         }
      case RuleAction::ScriptFunction:
         {
            if (OutsideScope_(_T("script function")))
               break;

            // Gated as a forward is: what the function does cannot be known -
            // it may send mail, write to a database, add a header with Add
            // rather than set one - and on a repeat pass it has already done
            // it, with any change it saved to the message still on disk.
            if (RepeatsAttempt_(account, pMsgData, _T("script function")))
               break;

            // A message too large to be read whole has only its header read here
            // (ApplyRules), and a function reads the message through the COM API's
            // Message, which does not read one of that size at all: its Subject,
            // its other header fields and its body would read as empty, though
            // the rule matched on them, and its Save is refused (HM6010). Until
            // the large-message row no rule ran on such a message, so no function
            // ran on one; until a script can read it, none does, and the log says
            // so where an administrator missing the function's work will look.
            if (pMsgData->IsHeaderOnly())
            {
               std::shared_ptr<Message> message = pMsgData->GetMessage();
               LOG_APPLICATION(Formatter::Format("RuleApplier - Message {0} is larger than the {1} MB a message is read whole at, so the script function {2} of rule \"{3}\" was not run: a script reads the message through the COM API, which does not read a message of that size - its header and its body would read as empty, and its Save would be refused. The rule's other actions are taken.",
                  message ? message->GetID() : 0,
                  (int) (MessageData::MaxParseBytes / (1024 * 1024)),
                  pAction->GetScriptFunction(),
                  pRule->GetName()));
               break;
            }

            ApplyAction_ScriptFunction(pAction, account, pMsgData);
            break;
         }
      case RuleAction::SetHeaderValue:
         {
            if (OutsideScope_(_T("header")))
               break;

            ApplyAction_SetHeader(pRule, pAction, account, pMsgData);
            break;
         }
      case RuleAction::StopRuleProcessing:
         {
            bContinueRuleProcessing = false;
            break;
         }
      case RuleAction::SendUsingRoute:
         {
            if (OutsideScope_(_T("route")))
               break;

            ruleResult.SetSendUsingRoute(pAction->GetRouteID());
            break;
         }
      case RuleAction::CreateCopy:  
         {
            if (RepeatsAttempt_(account, pMsgData, _T("copy")))
               break;

            ApplyAction_Copy(pRule, account, pMsgData);
            break;
         }
      case RuleAction::BindToAddress:
         {
            if (OutsideScope_(_T("bound address")))
               break;

            ruleResult.SetBindToAddress(pAction->GetValue());
            break;
         }
      case RuleAction::AddDisclaimer:
         {
            if (OutsideScope_(_T("disclaimer")))
               break;

            ApplyAction_AddDisclaimer(pRule, pAction, account, pMsgData);
            break;
         }
      case RuleAction::Reject:
         {
            // After acceptance there is nobody left to refuse to: the SMTP
            // session refused the message before accepting it where the rule
            // matched there (WouldReject), and the self-service send route
            // refuses before it queues (RejectedBySubmissionRules); here, on a
            // message that reached delivery another way - a scheduled send, a
            // marker an earlier rule wrote, a rule added while the message
            // waited in the queue - the rule drops it as a delete does. A
            // hosted sender is told, as warn_sender would tell them, that it
            // was not sent: the alternative is a Sent folder that says it was.
            // Only for the server's and the domains' rules, which are the
            // policy on what leaves; an account's rule is the recipient's own
            // filter, and a filter does not write to the people it filters.
            ruleResult.SetDeleteEmail(true);
            String sDeleteRuleName;
            sDeleteRuleName.Format(_T("Rule name: %s, ID: %d (reject)"), pRule->GetName().c_str(), pRule->GetID());
            ruleResult.SetDeleteRuleName(sDeleteRuleName);
            // In a recipient domain's scope the deliverer takes that domain's
            // recipients off and the message goes on to the others: the SMTP
            // session could not refuse it for them alone, so a hosted sender
            // is told here, by a notice that names those recipients and says
            // the rest of the message went (SendPolicyNotice_).
            if (!account)
               SendPolicyNotice_(pRule, pAction->GetValue(), pMsgData, true, account);
            break;
         }
      case RuleAction::RequireTLS:
         {
            if (OutsideScope_(_T("required TLS")))
               break;

            ruleResult.SetRequireTls(pRule->GetName());
            break;
         }
      case RuleAction::Quarantine:
         {
            ruleResult.SetQuarantine(pRule->GetName());
            break;
         }
      case RuleAction::WarnSender:
         {
            ApplyAction_WarnSender(pRule, pAction, account, pMsgData);
            break;
         }
      }
   }

   void
   RuleApplier::ApplyAction_WarnSender(std::shared_ptr<Rule> pRule, std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const
   {
      SendPolicyNotice_(pRule, pAction->GetValue(), pMsgData, false, account);
   }

   String
   RuleApplier::PolicyNoticeDomain_(std::shared_ptr<Rule> rule, std::shared_ptr<const Account> account, std::shared_ptr<Message> message,
                                    std::shared_ptr<const Domain> senderDomain) const
   {
      // An account's rule: the account's own domain.
      std::shared_ptr<const Account> owner = account;
      const __int64 ruleAccountId = rule && rule->GetAccountID() > 0 ? rule->GetAccountID() : rule_account_id_;
      if (!owner && ruleAccountId > 0)
         owner = CacheContainer::Instance()->GetAccount(ruleAccountId);
      if (owner)
      {
         std::shared_ptr<const Domain> ownerDomain = CacheContainer::Instance()->GetDomain(owner->GetDomainID());
         if (ownerDomain)
            return ownerDomain->GetName();

         return StringParser::ExtractDomain(owner->GetAddress());
      }

      // A domain's rule: the domain.
      const __int64 ruleDomainId = rule && rule->GetDomainID() > 0 ? rule->GetDomainID() : rule_domain_id_;
      if (ruleDomainId > 0)
      {
         std::shared_ptr<const Domain> ruleDomain = CacheContainer::Instance()->GetDomain(ruleDomainId);
         if (ruleDomain)
            return ruleDomain->GetName();
      }

      // A server rule, the server administrator's: the server's own domain when one is
      // configured and hosted here (DefaultDomain), else the domain the message was
      // addressed to, else - mail to outside alone - the sender's, which the server
      // runs as it runs every domain hosted here.
      const String defaultDomainName = Configuration::Instance()->GetDefaultDomain();
      if (!defaultDomainName.IsEmpty())
      {
         std::shared_ptr<const Domain> defaultDomain = CacheContainer::Instance()->GetDomain(defaultDomainName);
         if (defaultDomain)
            return defaultDomain->GetName();
      }

      if (message)
      {
         // owned by this call: the recipients of the message this delivery holds
         for (std::shared_ptr<MessageRecipient> recipient : message->GetRecipients()->GetVector())
         {
            if (!recipient || !recipient->GetIsLocalName())
               continue;

            std::shared_ptr<const Domain> receiving = CacheContainer::Instance()->GetDomain(StringParser::ExtractDomain(recipient->GetAddress()));
            if (receiving)
               return receiving->GetName();
         }
      }

      return senderDomain ? senderDomain->GetName() : String();
   }

   void
   RuleApplier::SendPolicyNotice_(std::shared_ptr<Rule> pRule, const String &actionText, std::shared_ptr<MessageData> pMsgData, bool notSent, std::shared_ptr<const Account> account) const
   {
      std::shared_ptr<Message> pOriginalMessage = pMsgData->GetMessage();
      if (!pOriginalMessage)
         return;

      // Once per message. The deliverer runs the server's and the domains'
      // rules on every attempt, and a recipient that greylists (a 451 on first
      // contact, the common case) or is down for an hour would otherwise mean
      // a notice per attempt, up to the route's number of tries. The first
      // pass is the one that tells the sender, as it is the one that
      // DKIM-signs and mirrors. A message the quarantine released is a new
      // queue row with a retry count of nought, so the flag the release sets
      // is what says its sender was told when it was held. Neither guard
      // applies to "not sent": that message is gone, and its sender has not
      // been told anything.
      // The counter, or the mark a shaping deferral leaves in its place: such
      // a deferral (SMTP/DestinationBackoff.h) keeps the counter at nought.
      //
      // Not for an account's own rule, which runs once for its recipient, in
      // local delivery, when that recipient's copy is stored: the marks below
      // travel on the message whichever pass delivers the recipient (see
      // RepeatsAttempt_), and the release mark is cleared on the recipient's
      // copy before its rules run, so for such a rule they said nothing about
      // whether this notice had gone - a recipient first delivered on a later
      // pass had its own rule's notice skipped although it had never run.
      const bool onceForTheMessage = !account;
      if (onceForTheMessage && !notSent && (pOriginalMessage->GetNoOfRetries() != 0 || pOriginalMessage->GetFirstPassDone()))
      {
         LOG_DEBUG("RuleApplier::SendPolicyNotice_: not the first delivery attempt, so the sender has been told already.");
         return;
      }
      if (onceForTheMessage && !notSent && pOriginalMessage->GetReleasedFromQuarantine())
      {
         LOG_DEBUG("RuleApplier::SendPolicyNotice_: the message was released from the quarantine, so the sender was told when it was held.");
         return;
      }

      // Only a sender this server hosts is told. A notice to an outside
      // sender about a policy of ours is backscatter: it goes to whoever the
      // envelope named, which for spam is somebody else.
      const String sender = pOriginalMessage->GetFromAddress();
      if (sender.IsEmpty())
         return;

      std::shared_ptr<const Domain> senderDomain = CacheContainer::Instance()->GetDomain(StringParser::ExtractDomain(sender));
      if (!senderDomain)
      {
         LOG_DEBUG("RuleApplier::SendPolicyNotice_: the sender is not hosted here, so no notice is sent.");
         return;
      }

      // An auto-submitted message is answered by nothing, and a message that
      // has been through the loop limit's worth of rule-generated mail gets
      // no more of it.
      if (pMsgData->IsAutoSubmitted())
      {
         LOG_DEBUG("RuleApplier::SendPolicyNotice_: the message is auto-submitted, so no notice is sent.");
         return;
      }
      if (!IsGeneratedResponseAllowed(pMsgData, false))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6820, "RuleApplier::SendPolicyNotice_", "Could not send the sender a policy notice: the maximum rule loop count was reached.");
         return;
      }

      // A reject in a recipient domain's scope refuses the message to that
      // domain's recipients alone (SMTPDeliverer::TakeDomainsRecipientsOff_),
      // so the notice names them and says the others were not affected.
      const bool notSentToSome = notSent && recipient_scope_domain_ != 0;

      String recipients;
      // owned by this call: the recipients of the message this delivery holds
      for (std::shared_ptr<MessageRecipient> recipient : pOriginalMessage->GetRecipients()->GetVector())
      {
         if (!recipient)
            continue;
         if (notSentToSome)
         {
            if (!recipient->GetIsLocalName())
               continue;

            std::shared_ptr<const Domain> recipientDomain = CacheContainer::Instance()->GetDomain(StringParser::ExtractDomain(recipient->GetAddress()));
            if (!recipientDomain || recipientDomain->GetID() != recipient_scope_domain_)
               continue;
         }
         if (!recipients.IsEmpty())
            recipients += _T(", ");
         recipients += recipient->GetAddress();
      }

      // The name as one line: it is a Subject and a header below, and a
      // Control Panel or COM rule name is not checked for a line break.
      const String ruleName = OneLine_(pRule->GetName());

      String text = actionText;
      text.Trim();

      String body;
      if (notSentToSome)
      {
         body.Format(_T("Your message with the subject \"%s\" was not delivered to %s: it matched the rule \"%s\" of their domain on this mail server, which refuses it.\r\n\r\n"),
            pMsgData->GetSubject().c_str(), recipients.c_str(), ruleName.c_str());
      }
      else if (notSent)
      {
         body.Format(_T("Your message to %s with the subject \"%s\" was not sent: it matched the outbound content policy \"%s\" of this mail server, which refuses it.\r\n\r\n"),
            recipients.c_str(), pMsgData->GetSubject().c_str(), ruleName.c_str());
      }
      else
      {
         body.Format(_T("Your message to %s with the subject \"%s\" matched the outbound content policy \"%s\" of this mail server.\r\n\r\n"),
            recipients.c_str(), pMsgData->GetSubject().c_str(), ruleName.c_str());
      }
      if (!text.IsEmpty())
         body += text + _T("\r\n\r\n");
      if (notSentToSome)
         body += _T("This notice is sent automatically. The message will not be tried again for those recipients; its other recipients are not affected by this rule.\r\n");
      else if (notSent)
         body += _T("This notice is sent automatically. The message has not been delivered to anyone and will not be tried again; a copy in your Sent folder is the only copy.\r\n");
      else
         body += _T("This notice is sent automatically. What happened to the message is decided by the rule's other actions; a rule with no other action delivered it.\r\n");

      std::shared_ptr<Message> pMsg = std::shared_ptr<Message>(new Message());
      pMsg->SetState(Message::Delivering);

      String newMessageFileName = MessageStore::WorkingFile(pMsg);

      std::shared_ptr<MessageData> pNewMsgData = std::shared_ptr<MessageData>(new MessageData());
      pNewMsgData->LoadFromMessage(newMessageFileName, pMsg);
      pNewMsgData->GenerateMessageID();
      pNewMsgData->SetTo(sender);
      // From the postmaster of the domain whose rule it is (PolicyNoticeDomain_): the
      // notice carries the rule's name and its owner's text, which are that domain's
      // words, and the server signs it as the domain its From names. It came from the
      // postmaster of the sender's own domain, so that an account's or a domain's rule
      // spoke to a sender of another domain hosted here in that domain's name, signed
      // with its key.
      const String noticeDomain = PolicyNoticeDomain_(pRule, account, pOriginalMessage, senderDomain);
      pNewMsgData->SetFrom(_T("postmaster@") + noticeDomain);
      pNewMsgData->SetSubject(_T("Policy notice: ") + ruleName);
      pNewMsgData->SetBody(body);
      pNewMsgData->SetSentTime(Time::GetCurrentMimeDate());
      pNewMsgData->SetFieldValue(_T("X-hMailServer-Policy"), ruleName);
      pNewMsgData->SetFieldValue(_T("X-hMailServer-Policy-Outcome"), notSent ? _T("not-sent") : _T("delivered"));
      pNewMsgData->SetAutoGenerated();

      RuleGuard::CarryLoopCountForward(pMsgData, pNewMsgData);

      if (!pNewMsgData->WriteReported(newMessageFileName, "A rule's notice to the sender"))
         return;

      bool recipientOK = false;
      RecipientParser recipientParser;
      recipientParser.CreateMessageRecipientList(sender, pMsg->GetRecipients(), recipientOK);

      // Two failures with two causes, told apart as the forward action tells
      // its own apart: a sender in a hosted domain whose address is no
      // mailbox, alias or list here (a MAIL FROM an outside connection was
      // allowed to give), and a queue row that could not be written.
      if (pMsg->GetRecipients()->GetCount() == 0)
      {
         FileUtilities::DeleteFile(newMessageFileName);

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6823, "RuleApplier::SendPolicyNotice_",
            Formatter::Format("Could not send the sender a policy notice: {0} is in a domain this server hosts but is not a mailbox, an alias or a list here, so there is nowhere to deliver it. The notice file has been removed.", sender));
         return;
      }

      if (!PersistentMessage::SaveObject(pMsg))
      {
         FileUtilities::DeleteFile(newMessageFileName);

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6824, "RuleApplier::SendPolicyNotice_", "Could not send the sender a policy notice; the notice could not be saved to the queue. The file has been removed rather than left on disk with nothing referring to it.");
         return;
      }

      DeliverQueuedMessageNow_();

      LOG_APPLICATION(Formatter::Format("RuleApplier - Message {0}: the sender {1} has been sent a notice naming the rule \"{2}\"{3}.",
         pOriginalMessage->GetID(), sender, ruleName, notSent ? String(_T(", saying the message was not sent")) : String()));
   }

   void
   RuleApplier::ApplyAction_AddDisclaimer(std::shared_ptr<Rule> pRule, std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const
   {
      const String text = pAction->GetBody();
      if (text.IsEmpty())
         return;

      std::shared_ptr<MessageData> data = pMsgData;
      if (!DisclaimerAdder::AppendText(pMsgData->GetMessage(), text, data, _T("rule ") + pRule->GetName()))
         return;

      String fileName = MessageStore::WorkingFile(account, pMsgData->GetMessage());
      if (!pMsgData->Write(fileName))
         RuleGuard::ReportActionFailed(_T("AddDisclaimer"), fileName);
   }

   bool
   RuleApplier::RuleMatches_(std::shared_ptr<Rule> pRule, std::shared_ptr<MessageData> pMsgData)
   {
      const bool bAllRequired = pRule->GetUseAND();
      bool matched = false;

      std::shared_ptr<RuleCriterias> pCriterias = pRule->GetCriterias();
      for (int i = 0; i < pCriterias->GetCount(); i++)
      {
         std::shared_ptr<RuleCriteria> pCriteria = pCriterias->GetItem(i);
         if (!pCriteria)
            continue;

         bool sealedBodyDeclined = false;
         bool unreadBodyDeclined = false;
         const bool bMatch = MessageMatchesCriteria_(pCriteria, pMsgData, sealedBodyDeclined, unreadBodyDeclined);

         if (bAllRequired)
         {
            if (!bMatch)
               return false;
            matched = true;
         }
         else if (bMatch)
            return true;
      }

      return matched;
   }

   bool
   RuleApplier::HasRejectAction(std::shared_ptr<Rules> pRules)
   {
      if (!pRules)
         return false;

      // A locked copy, never GetVector(). The collections this is handed are
      // ObjectCache's global and domain rules (RejectedBySubmissionRules),
      // shared by every session and every delivery. A domain's is refreshed IN
      // PLACE - GetDomainRules calls Refresh() on the cached object, on
      // whichever thread asks first after a rule was saved - and the global
      // ones by a restore (Configuration.cpp), so the vector a range-for over
      // GetVector() walks can be swapped out and freed under it.
      for (std::shared_ptr<Rule> pRule : pRules->GetSnapshot())
      {
         if (!pRule || !pRule->GetActive())
            continue;

         std::shared_ptr<RuleActions> pActions = pRule->GetActions();
         for (int i = 0; i < pActions->GetCount(); i++)
         {
            std::shared_ptr<RuleAction> pAction = pActions->GetItem(i);
            if (pAction && pAction->GetType() == RuleAction::Reject)
               return true;
         }
      }

      return false;
   }

   bool
   RuleApplier::WouldReject(std::shared_ptr<Rules> pRules, std::shared_ptr<MessageData> pMsgData, String &text)
   {
      if (!pRules || !pMsgData)
         return false;

      // A locked copy, as HasRejectAction takes one and for its reason.
      for (std::shared_ptr<Rule> pRule : pRules->GetSnapshot())
      {
         if (!pRule || !pRule->GetActive())
            continue;

         std::shared_ptr<RuleActions> pActions = pRule->GetActions();
         std::shared_ptr<RuleAction> reject;
         for (int i = 0; i < pActions->GetCount(); i++)
         {
            std::shared_ptr<RuleAction> pAction = pActions->GetItem(i);
            if (pAction && pAction->GetType() == RuleAction::Reject)
            {
               reject = pAction;
               break;
            }
         }

         if (!reject || !RuleMatches_(pRule, pMsgData))
            continue;

         // One line, whatever was typed into the action or the rule's name:
         // this is the text of a single 550 reply, and a line break in it
         // would be a second reply of the sender's choosing.
         text = OneLine_(reject->GetValue());
         text.Trim();
         // Without a text of its own the refusal names the rule, so the
         // sender is told which policy stopped the message rather than that
         // one did.
         if (text.IsEmpty())
            text = _T("Message refused by the rule \"") + OneLine_(pRule->GetName()) + _T("\" of this server.");
         return true;
      }

      return false;
   }

   bool
   RuleApplier::RejectedBySubmissionRules(std::shared_ptr<Message> message, String &text)
   {
      if (!message)
         return false;

      // The collections first, because they are in memory: only when one of
      // them has a reject action at all is the message parsed for the match.
      std::vector<std::shared_ptr<Rules> > collections;
      collections.push_back(ObjectCache::Instance()->GetGlobalRules());

      // The domains whose rules act on the whole message - the same choice
      // the deliverer makes (DomainRuleScopes). A domain whose rules reach
      // only its own recipients cannot refuse the message for the others: its
      // reject is met at delivery, where its recipients are taken off.
      for (const DomainRuleScope &scope : DomainRuleScopes(message))
      {
         if (scope.whole_message)
            collections.push_back(ObjectCache::Instance()->GetDomainRules(scope.domain_id));
      }

      bool anyReject = false;
      for (std::shared_ptr<Rules> rules : collections)
         anyReject = anyReject || HasRejectAction(rules);

      if (!anyReject)
         return false;

      // Header only for a message too large to be read whole, as ApplyRules reads
      // it, so that a reject rule on its header refuses it here rather than at
      // delivery.
      std::shared_ptr<MessageData> data = std::shared_ptr<MessageData>(new MessageData());
      std::shared_ptr<const Account> noAccount;
      if (!data->LoadFromMessage(noAccount, message, MessageData::LargeMessage::HeaderOnly))
         return false;

      RuleApplier applier;
      for (std::shared_ptr<Rules> rules : collections)
      {
         if (applier.WouldReject(rules, data, text))
            return true;
      }

      return false;
   }

   void
   RuleApplier::RecordRelayingDomain_(std::shared_ptr<const Account> account, std::shared_ptr<Message> copy, std::shared_ptr<Message> source) const
   {
      // An account's rule: CopyToQueue recorded the account's domain.
      if (account)
         return;

      if (rule_account_id_ > 0)
      {
         std::shared_ptr<const Account> ruleAccount = CacheContainer::Instance()->GetAccount(rule_account_id_);
         if (ruleAccount)
            copy->SetRelayingDomainID(ruleAccount->GetDomainID());
         return;
      }

      // A domain's rule sends the copy on for that domain; a server rule for the
      // domain the message was received for, or whose account submitted it. Never a
      // domain a header names (Message::GetRelayingDomainID).
      if (rule_domain_id_ > 0)
         copy->SetRelayingDomainID(rule_domain_id_);
      else
         copy->SetRelayingDomainID(DKIMSigner::RelayingDomainID(source, true));
   }

   void
   RuleApplier::ApplyAction_Forward(std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const
   {
      // false = check only loop counter not AutoSubmitted header because forward
      if (!IsGeneratedResponseAllowed(pMsgData, false))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5064, "RuleApplier::ApplyAction_Forward", "Could not forward message. Maximum rule loop count reached.");
         return;
      }

      if (pAction->GetTo().IsEmpty())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4225, "RuleApplier::ApplyAction_Forward", "Could not forward message; empty recipient address.");
         return;
      }

      std::shared_ptr<Message> pOriginalMessage = pMsgData->GetMessage();

      if (!pOriginalMessage)
         return;

      if (pOriginalMessage->GetFlagSpam() && pAction->GetAbortSpamFlagged())
      {
         LOG_DEBUG("RuleApplier::ApplyAction_Forward aborted, message marked as spam");
         return; // skip message flagged as spam
      }

      // Restricted delivery (Common/Util/RestrictedDelivery.h): an account
      // rule's forward is the account sending, one step removed, and a
      // restricted account may not forward to a domain it may not write to.
      // The message stays in the mailbox; the log says why the copy was not
      // made. A global rule has no account and is the administrator's.
      {
         std::shared_ptr<const Account> ruleAccount = account;
         if (!ruleAccount && rule_account_id_ > 0)
            ruleAccount = CacheContainer::Instance()->GetAccount(rule_account_id_);

         if (ruleAccount && RestrictedDelivery::Check(ruleAccount, pAction->GetTo(), RestrictedDelivery::Outbound) == RestrictedDelivery::Refused)
         {
            LOG_DEBUG("RuleApplier::ApplyAction_Forward aborted: restricted delivery - " + ruleAccount->GetAddress() +
               " may send to named domains only, and " + pAction->GetTo() + " is not in one. The message stays in the mailbox.");
            return;
         }
      }

      // The envelope sender of the copy (SMTPForwarding::EnvelopeOfCopy): the address of
      // the account whose rule it is when RewriteEnvelopeFromWhenForwarding asks for it,
      // or the message's own. A copy that may not be sent on with the one it came with -
      // the null sender under this server's EHLO name, or one at that name or at a
      // domain hosted here - goes to no address outside (below).
      String envelope;
      bool maySendOn = true;
      {
         std::shared_ptr<const Account> ruleAccount = CacheContainer::Instance()->GetAccount(rule_account_id_);
         const String envelopeSender = pOriginalMessage->GetFromAddress();
         const String rewritten = ruleAccount && IniFileSettings::Instance()->GetRewriteEnvelopeFromWhenForwarding() && !envelopeSender.IsEmpty()
            ? ruleAccount->GetAddress() : String();

         std::vector<String> fromFieldValues;
         if (pOriginalMessage->GetFromOutside() && pMsgData->GetMimeMessage())
            fromFieldValues = SMTPForwarding::FromFieldValues(*pMsgData->GetMimeMessage());

         const bool senderOrganisationHosted = pOriginalMessage->GetFromOutside() && !envelopeSender.IsEmpty() &&
                                               SMTPForwarding::IsHostedOrganisation(StringParser::ExtractDomain(envelopeSender));

         bool rewriteWithheld = false;
         maySendOn = SMTPForwarding::EnvelopeOfCopy(pOriginalMessage->GetFromOutside(), pOriginalMessage->GetAuthDomainsOnArrival(), envelopeSender,
                                                    senderOrganisationHosted, fromFieldValues, rewritten, Utilities::ComputerName(), envelope,
                                                    rewriteWithheld);

         if (rewriteWithheld)
         {
            LOG_DEBUG(Formatter::Format("RuleApplier: the forward of message {0} keeps the envelope sender it came with, <{1}>, in place of {2}: the message came from outside with a From of that address's organisation, with no DMARC pass for it recorded when it arrived.",
               pOriginalMessage->GetID(), envelopeSender, rewritten));
         }
      }

      std::shared_ptr<Message> pMsg = PersistentMessage::CopyToQueue(account, pMsgData->GetMessage());

      if (!pMsg)
         return;

      RecordRelayingDomain_(account, pMsg, pMsgData->GetMessage());

      pMsg->SetState(Message::Delivering);

      std::shared_ptr<Account> emptyAccount;

      // Increase the number of rule-deliveries made.
      String newFileName = MessageStore::WorkingFile(emptyAccount, pMsg);

      // Header only when the copy is too large to be read whole: the count is a
      // header field, and the body is copied behind it as it is.
      std::shared_ptr<MessageData> pNewMsgData = std::shared_ptr<MessageData>(new MessageData());
      pNewMsgData->LoadFromMessage(emptyAccount, pMsg, MessageData::LargeMessage::HeaderOnly);
      pNewMsgData->IncreaseRuleLoopCount();
      pNewMsgData->WriteReported(newFileName, "The rule-loop counter on a forwarded message");

      // The envelope sender chosen above: for a user-level account's rule, its address
      // when the setting asks for it and the message may be sent on with it.
      if (pMsg->GetFromAddress().Compare(envelope) != 0)
         pMsg->SetFromAddress(envelope);

      // Add new recipients
      bool recipientOK = false;
      RecipientParser recipientParser;
      recipientParser.CreateMessageRecipientList(pAction->GetTo(), pMsg->GetRecipients(), recipientOK);

      // A copy that may not be sent on goes to the addresses here alone, and when there
      // are none it is not made: the message stays where it is.
      if (!maySendOn && SMTPForwarding::WithholdOutsideRecipients(pMsg, pOriginalMessage->GetID(), _T("rule's forward"), envelope) &&
          pMsg->GetRecipients()->GetCount() == 0)
      {
         FileUtilities::DeleteFile(newFileName);
         return;
      }

      // Check that there are recipients of the letter. If not, we should skip delivery.
      if (pMsg->GetRecipients()->GetCount() == 0)
      {
         // Delete the file since the message cannot be delivered.
         FileUtilities::DeleteFile(newFileName);

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4235, "RuleApplier::ApplyAction_Forward", "Could not forward message; no recipients.");
         return;
      }

      // Unchecked, so a failed save left the forwarded copy's file on disk with no
      // row referring to it and the forward silently not happening. The original
      // message is untouched either way - this creates a second one - so the cost is
      // an orphaned file and a rule that quietly did not run, not lost mail. Handled
      // the same way as the no-recipients case immediately above.
      if (!PersistentMessage::SaveObject(pMsg))
      {
         FileUtilities::DeleteFile(newFileName);

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6082, "RuleApplier::ApplyAction_Forward", "Could not forward message; the forwarded copy could not be saved. The file has been removed rather than left on disk with nothing referring to it.");
         return;
      }

      DeliverQueuedMessageNow_();
   }

   void 
   RuleApplier::ApplyAction_Copy(std::shared_ptr<Rule> rule, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const
   {
      // false = check only loop counter not AutoSubmitted header because forwarding copy
      if (!IsGeneratedResponseAllowed(pMsgData, false))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5201, "RuleApplier::ApplyAction_Copy", "Could not copy message. Maximum rule loop count reached.");
         return;
      }

      std::shared_ptr<Message> pMsg = PersistentMessage::CopyToQueue(account, pMsgData->GetMessage());

      if (!pMsg)
         return;

      RecordRelayingDomain_(account, pMsg, pMsgData->GetMessage());

      // We've created a copy of the message, but we need to copy the recipients as well.
      // The recipients are either the full list of SMTP envelope recipients, or, if it's
      // an account level rule it's just the address of the account. To determine this, we
      // check whether the message has been connected to any account yet.
      if (pMsgData->GetMessage()->GetAccountID() > 0)
      {
         // What account is it in?
         std::shared_ptr<const Account> recipientAccount = CacheContainer::Instance()->GetAccount(pMsgData->GetMessage()->GetAccountID());

         if (recipientAccount)
         {
            bool recipientOK = false;

            RecipientParser recipientParser;
            recipientParser.CreateMessageRecipientList(recipientAccount->GetAddress(), pMsg->GetRecipients(), recipientOK);
         }
      }
      else
      {
         // Copy the old reciopients.
         // owned by this call: the recipients of the message this delivery holds
         std::vector<std::shared_ptr<MessageRecipient> >  &oldRecipients = pMsgData->GetMessage()->GetRecipients()->GetVector();

         for(std::shared_ptr<MessageRecipient> recipient : oldRecipients)
         {
            // A recipient domain's rule copies to its own recipients: a copy
            // to everybody would deliver the message twice to recipients the
            // domain has no say over.
            if (recipient_scope_domain_ != 0)
            {
               if (!recipient || !recipient->GetIsLocalName())
                  continue;

               std::shared_ptr<const Domain> recipientDomain = CacheContainer::Instance()->GetDomain(StringParser::ExtractDomain(recipient->GetAddress()));
               if (!recipientDomain || recipientDomain->GetID() != recipient_scope_domain_)
                  continue;
            }

            std::shared_ptr<MessageRecipient> newRecipient = std::shared_ptr<MessageRecipient >(new MessageRecipient());
            newRecipient->CopyFrom(recipient);
            newRecipient->SetMessageID(0);

            pMsg->GetRecipients()->Add(newRecipient);
         }
      }

      pMsg->SetState(Message::Delivering);

      // Increase the number of rule-deliveries made.
      String newMessageFileName = MessageStore::WorkingFile(pMsg);

      // Header only when the copy is too large to be read whole, as for a forward.
      std::shared_ptr<MessageData> pNewMsgData = std::shared_ptr<MessageData>(new MessageData());
      pNewMsgData->LoadFromMessage(newMessageFileName, pMsg, MessageData::LargeMessage::HeaderOnly);
      pNewMsgData->IncreaseRuleLoopCount();
      pNewMsgData->SetFieldValue("X-CopyRule", rule->GetName());
      pNewMsgData->WriteReported(newMessageFileName, "The X-CopyRule header and rule-loop counter on a copied message");

      // Check that there are recipients of the letter. If not, we should skip delivery.
      if (pMsg->GetRecipients()->GetCount() == 0)
      {
         // Delete the file since the message cannot be delivered.
         FileUtilities::DeleteFile(newMessageFileName);

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5201, "RuleApplier::ApplyAction_Copy", "Could not copy message; no recipients.");
         return;
      }

      // As in ApplyAction_Forward above.
      if (!PersistentMessage::SaveObject(pMsg))
      {
         FileUtilities::DeleteFile(newMessageFileName);

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6083, "RuleApplier::ApplyAction_Copy", "Could not copy message; the copy could not be saved. The file has been removed rather than left on disk with nothing referring to it.");
         return;
      }

      DeliverQueuedMessageNow_();
   }

   void 
   RuleApplier::ApplyAction_ScriptFunction(std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const
   {
      // Run a custom function
      String sFunctionName = pAction->GetScriptFunction();

      std::shared_ptr<ScriptObjectContainer> pContainer = std::shared_ptr<ScriptObjectContainer>(new ScriptObjectContainer);
      std::shared_ptr<Result> pResult = std::shared_ptr<Result>(new Result);
      pContainer->AddObject("HMAILSERVER_MESSAGE", pMsgData->GetMessage(), ScriptObject::OTMessage);
      pContainer->AddObject("Result", pResult, ScriptObject::OTResult);

      // The function by name, for the JavaScript engine, which looks it up and
      // calls it rather than running the text below.
      pContainer->SetProcedureName(sFunctionName);

      String sEventCaller = sFunctionName + "(HMAILSERVER_MESSAGE)";
      ScriptServer::Instance()->FireEvent(ScriptServer::EventCustom, sEventCaller, pContainer);

      // The script may have modified the message. We need to reload it. We could
      // do a CRC check or something similar to determine whether it has changed,
      // but performance-wise we can probably just as well reload it - as ApplyRules
      // loads it, header only when it is too large to be read whole. No function
      // is run on a message of that size (ApplyAction_), but one can make a
      // message that size, by adding an attachment, and the rules after this one
      // then still have its header, where a refused load left them an empty
      // message to compare their criteria with.
      pMsgData->LoadFromMessage(account, pMsgData->GetMessage(), MessageData::LargeMessage::HeaderOnly);
   }

   void 
   RuleApplier::ApplyAction_SetHeader(std::shared_ptr<Rule> pRule, std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const
   {
      String sHeader = pAction->GetHeaderName();
      String sValue = pAction->GetValue();

      // Not X-hMailServer-LoopCount, which the forward or reply after this action
      // reads - set to 0 it gave every copy the whole rule loop limit again - and not
      // a name that would write a field of another name; and, for a domain's or an
      // account's rule, not From, Sender, the Resent- pair or DKIM-Signature, which a
      // forward, a copy or a reply would go out in, signed as the domain they name.
      // Save refuses all of them now; this is for an action stored before it did,
      // which is skipped while the rule's other actions are taken
      // (RuleGuard::WhyHeaderMayNotBeSet).
      const String refusal = RuleGuard::WhyHeaderMayNotBeSet(sHeader, pRule ? pRule->GetAccountID() : 0, pRule ? pRule->GetDomainID() : 0);
      if (!refusal.IsEmpty())
      {
         std::shared_ptr<Message> message = pMsgData->GetMessage();
         LOG_APPLICATION(Formatter::Format("RuleApplier: the Set header value action of rule {0} was not applied to message {1}: {2}",
            pAction->GetRuleID(), message ? message->GetID() : 0, refusal));
         return;
      }

      MimeHeader mimeHeader;
      AnsiString header = pMsgData->GetHeader();
      mimeHeader.Load(header, header.GetLength(), true);

      // Replace macro value
      sValue.Replace(_T("%MACRO_ORIGINAL_HEADER%"), String(mimeHeader.GetRawFieldValue(sHeader)));

      // One field, whatever the value holds: a line break in it wrote a second field,
      // X-hMailServer-LoopCount among the names it could have.
      sValue = RuleGuard::HeaderValueAsOneField(sValue);

      pMsgData->SetFieldValue(sHeader, sValue);

      String fileName = MessageStore::WorkingFile(account, pMsgData->GetMessage());

      // Write is not atomic - MimeBody::SaveAllToFile rewrites the file in place - so a
      // failure part-way through leaves a message that is neither the original nor the
      // rewritten one, and delivery continues with it. This does not repair that; it
      // stops it being silent, which it has been since the action was written.
      if (!pMsgData->Write(fileName))
         RuleGuard::ReportActionFailed(_T("SetHeaderValue"), fileName);
   }

   void 
   RuleApplier::ApplyAction_Reply(std::shared_ptr<Rule> pRule, std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const
   {
      // true = check AutoSubmitted header and do not respond if set
      if (!IsGeneratedResponseAllowed(pMsgData, true))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5065, "RuleApplier::ApplyAction_Reply", "Could not reply message. Maximum rule loop count reached or Auto-Submitted header.");
         return;
      }

      String sReplyRecipientAddress = pMsgData->GetMessage()->GetFromAddress();

      if (sReplyRecipientAddress.IsEmpty())
      {
         // We need a recipient address to be able to
         // send the message..
         return;
      }

      std::shared_ptr<Message> pOriginalMessage = pMsgData->GetMessage();

      if (!pOriginalMessage)
         return;

      if (pOriginalMessage->GetFlagSpam() && pAction->GetAbortSpamFlagged())
      {
         LOG_DEBUG("RuleApplier::ApplyAction_Reply aborted, message marked as spam");
         return; // skip message flagged as spam
      }

      // From an address the rule's owner may send as: for an account's rule, what the
      // account may send as; for a domain's, an address of the domain. Save refuses
      // any other now, and this is for an action stored before it did - the reply is
      // not sent, rather than sent in somebody else's name and signed as their domain
      // (RuleGuard::WhyReplyFromMayNotBeUsed).
      {
         const String refusal = RuleGuard::WhyReplyFromMayNotBeUsed(pRule ? pRule->GetAccountID() : 0, pRule ? pRule->GetDomainID() : 0, pAction->GetFromAddress());
         if (!refusal.IsEmpty())
         {
            LOG_APPLICATION(Formatter::Format("RuleApplier: the Reply action of rule {0} was not applied to message {1}: {2}",
               pAction->GetRuleID(), pOriginalMessage->GetID(), refusal));
            return;
         }
      }

      // Reply to the email
      std::shared_ptr<Message> pMsg = std::shared_ptr<Message>(new Message());
      pMsg->SetState(Message::Delivering);

      String newMessageFileName = MessageStore::WorkingFile(pMsg);

      std::shared_ptr<MessageData> pNewMsgData = std::shared_ptr<MessageData>(new MessageData());
      pNewMsgData->LoadFromMessage(newMessageFileName, pMsg);
      pNewMsgData->GenerateMessageID();
      pNewMsgData->SetTo(sReplyRecipientAddress);
      // The name as a quoted string, so that a name with a comma or an address in it
      // adds no address to the From (RuleGuard::ReplyFromHeader).
      pNewMsgData->SetFrom(RuleGuard::ReplyFromHeader(pAction->GetFromName(), pAction->GetFromAddress()));
      pNewMsgData->SetSubject(pAction->GetSubject());
      pNewMsgData->SetBody(pAction->GetBody());
      pNewMsgData->SetSentTime(Time::GetCurrentMimeDate());
      pNewMsgData->SetAutoReplied();

      // Carry the count forward from the message that triggered the reply. pMsg is a
      // brand-new Message, so IncreaseRuleLoopCount() here always produced 1 whatever
      // the original carried, and the RuleLoopLimit bound restarted at every reply.
      RuleGuard::CarryLoopCountForward(pMsgData, pNewMsgData);

      // Unlike the forward and copy actions, this is the FIRST write of a message built
      // from nothing, so a failure here is not "a header is missing from an existing
      // message" - there is no message. Queueing it anyway, which is what discarding the
      // result did, puts a row in the queue whose file is not there and leaves the
      // delivery to discover it. Abort instead: no reply is better than a reply the
      // recipient will never be able to read, and the function already returns early on
      // two other conditions.
      if (!pNewMsgData->WriteReported(newMessageFileName, "A rule-generated reply message"))
         return;

      // Add recipients.
      bool recipientOK = false;
      RecipientParser recipientParser;
      recipientParser.CreateMessageRecipientList(sReplyRecipientAddress, pMsg->GetRecipients(), recipientOK);

      // As above, and for the same reason the WriteReported check a few lines up
      // exists: a row without its file, or here a file without its row, is worse than
      // no reply at all.
      if (!PersistentMessage::SaveObject(pMsg))
      {
         FileUtilities::DeleteFile(newMessageFileName);

         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6084, "RuleApplier::ApplyAction_Reply", "Could not send the rule-generated reply; it could not be saved. The file has been removed rather than left on disk with nothing referring to it.");
         return;
      }

      DeliverQueuedMessageNow_();
   }

   bool
   RuleApplier::MessageMatchesCriteria_(std::shared_ptr<RuleCriteria> pCriteria, std::shared_ptr<MessageData> pMsgData, bool &sealedBodyDeclined, bool &unreadBodyDeclined) const
   {
      String sFieldValue;
      if (pCriteria->GetUsePredefined())
      {
         // A message too large to be read whole has only its header read
         // (ApplyRules), so its body is not empty but unread, and neither are its
         // attachments absent: as for a sealed body below, a criterion that needs
         // them does not match, whichever way round it is phrased - a "body does
         // not contain" or an "attachment name does not contain .exe" would
         // otherwise match every such message. The size, the recipients and every
         // header are read as for any message.
         switch (pCriteria->GetPredefinedField())
         {
         case RuleCriteria::FTBody:
         case RuleCriteria::FTAttachmentName:
         case RuleCriteria::FTSensitiveContent:
         case RuleCriteria::FTContent:
         case RuleCriteria::FTAttachmentType:
            if (pMsgData->IsHeaderOnly())
            {
               unreadBodyDeclined = true;
               return false;
            }
            break;
         default:
            break;
         }

         switch (pCriteria->GetPredefinedField())
         {
         case RuleCriteria::FTFrom:
            sFieldValue = pMsgData->GetFrom();
            break;
         case RuleCriteria::FTTo:
            sFieldValue = pMsgData->GetTo();
            break;
         case RuleCriteria::FTCC:
            sFieldValue = pMsgData->GetCC();
            break;
         case RuleCriteria::FTSubject:
            sFieldValue = pMsgData->GetSubject();
            break;
         case RuleCriteria::FTBody:
            // The body of a sealed message is not empty. It is unreadable, and
            // those are different facts that this criterion used to state
            // identically.
            //
            // GetBody() walks the MIME tree for a text/plain part and
            // GetHTMLBody() for a text/html one; a multipart/encrypted message
            // has neither, so the pair came back as "" - and an empty string is
            // a perfectly good value to compare against. "Contains" then missed,
            // harmlessly, while "NotContains" matched EVERY sealed message. A
            // compliance rule of the shape "body does not contain our
            // confidentiality notice -> hold" therefore held every encrypted
            // message the organisation sent or received, silently, and the more
            // seriously a company took encryption the more of its mail
            // disappeared.
            //
            // This is the server's own rule engine rather than a standard, so
            // the semantics are ours to choose: a criterion the server cannot
            // evaluate does not match, whichever way round it is phrased. The
            // rule's other criteria are still evaluated - a criterion declining
            // is not the whole rule declining - so "From contains x OR body
            // contains y" still fires on the From.
            //
            // IsOpaque and not IsSealed, on purpose. A multipart/signed message
            // has its text in the clear, GetBody() reads it, and a rule on it
            // has always evaluated honestly; declining there would switch off
            // every body rule an organisation has on its own signed mail, which
            // is the opposite of what this fix is for.
            //
            // And IsOpaque reads the body, not the label: a multipart/encrypted
            // header over ordinary text parts is one line anybody can write,
            // and declining on it would let a message carry past every body
            // and content rule exactly the text those rules exist to find.
            if (SealedMessage::IsOpaque(pMsgData))
            {
               sealedBodyDeclined = true;
               return false;
            }

            sFieldValue = pMsgData->GetBody() + pMsgData->GetHTMLBody();
            break;
         case RuleCriteria::FTMessageSize:
            sFieldValue.Format(_T("%d"), pMsgData->GetSize());
            break;
         case RuleCriteria::FTDeliveryAttempts:
            sFieldValue.Format(_T("%u"), pMsgData->GetMessage()->GetNoOfRetries() + 1);
            break;
         case RuleCriteria::FTAttachmentName:
            {
               // Every attachment's file name, ';' between them, so "contains
               // .exe", a wildcard "*.zip" or a regex on the list all read as
               // "any attachment is".
               std::shared_ptr<Attachments> attachments = pMsgData->GetAttachments();
               if (attachments)
               {
                  for (size_t i = 0; i < attachments->GetCount(); i++)
                  {
                     std::shared_ptr<Attachment> attachment = attachments->GetItem((unsigned int) i);
                     if (!attachment)
                        continue;
                     if (!sFieldValue.IsEmpty())
                        sFieldValue += ";";
                     sFieldValue += attachment->GetFileName();
                  }
               }
               break;
            }
         case RuleCriteria::FTSensitiveContent:
         case RuleCriteria::FTContent:
            {
               // The readable text, or the names of the detectors that found
               // something in it. A sealed message has none the server can
               // read, and declines exactly as the body criterion does above.
               if (SealedMessage::IsOpaque(pMsgData))
               {
                  sealedBodyDeclined = true;
                  return false;
               }

               const String text = ContentPolicy::ReadableText(pMsgData);
               sFieldValue = pCriteria->GetPredefinedField() == RuleCriteria::FTContent
                  ? text
                  : ContentPolicy::Detectors(text);
               break;
            }
         case RuleCriteria::FTAttachmentType:
            sFieldValue = ContentPolicy::AttachmentTypes(pMsgData);
            break;
         case RuleCriteria::FTExternalRecipient:
            sFieldValue = ContentPolicy::ExternalRecipients(pMsgData->GetMessage());
            break;
         case RuleCriteria::FTRecipientList:
            {
               std::shared_ptr<Message> pMessage = pMsgData->GetMessage();
               // owned by this call: the recipients of the message this delivery holds
               std::vector<std::shared_ptr<MessageRecipient> > vecRecipients = pMessage->GetRecipients()->GetVector();
               auto iterRecipient = vecRecipients.begin();

               while (iterRecipient != vecRecipients.end())
               {
                  if (!sFieldValue.IsEmpty())
                     sFieldValue += ";";

                  sFieldValue += (*iterRecipient)->GetAddress();
                  iterRecipient++;
               }

               break;
            }
         }
      }
      else
      {
         sFieldValue = pMsgData->GetFieldValue(pCriteria->GetHeaderField());
      }

      String matchValue = pCriteria->GetMatchValue();


      return TestMatch(matchValue, pCriteria->GetMatchType(), sFieldValue);
   }

   bool 
   RuleApplier::TestMatch(String matchValue, RuleCriteria::MatchType matchType, String testValue)
   {
      switch (matchType)
      {
      case RuleCriteria::Equals:
         {
            if (matchValue.CompareNoCase(testValue) == 0)
               return true;
            else
               return false;
         }
      case RuleCriteria::NotEquals:
         {
            if (matchValue.CompareNoCase(testValue) != 0)
               return true;
            else
               return false;
         }
      case RuleCriteria::Contains:
         {
            if (testValue.FindNoCase(matchValue) >= 0)
               return true;
            else
               return false;
         }
      case RuleCriteria::NotContains:
         {
            if (testValue.FindNoCase(matchValue) == -1)
               return true;
            else
               return false;
         }

      case RuleCriteria::LessThan:
         {
            testValue.Replace(_T(","), _T("."));
            matchValue.Replace(_T(","), _T("."));

            double iFieldValue =  _tstof(testValue);
            double iMatchValue = _tstof(matchValue);

            if (iMatchValue > iFieldValue)
               return true;
            else
               return false;
         }
      case RuleCriteria::GreaterThan:
         {
            testValue.Replace(_T(","), _T("."));
            matchValue.Replace(_T(","), _T("."));

            double iFieldValue = _tstof(testValue);
            double iMatchValue = _tstof(matchValue);

            if (iMatchValue < iFieldValue)
               return true;
            else
               return false;
         }
      case RuleCriteria::MatchesRegEx:
         {
            // Not RegularExpression::TestExactMatch any more. That function collapses
            // "this pattern will not compile" and "Boost abandoned the match" into the
            // same silent false, and both of those need saying out loud - see RuleGuard.
            // The construction and the match itself are unchanged.
            return RuleGuard::RegexCriteriaMatches(matchValue, testValue);
         }
      case RuleCriteria::Wildcard:
         {
            if (StringParser::WildcardMatchNoCase(matchValue, testValue))
               return true;
            else
               return false;

         }
      default:
         {
            HM_ASSERT(0); // unknown match type?
            return false;
         }


      }

      return false;
   }

   bool
   RuleApplier::IsGeneratedResponseAllowed(std::shared_ptr<MessageData> pMsgData, bool bChkAutoSubmit)
   {
      int iCurrProcessCount = pMsgData->GetRuleLoopCount();
      int iMaxAllowed = Configuration::Instance()->GetSMTPConfiguration()->GetRuleLoopLimit();

      if (iCurrProcessCount >= iMaxAllowed)
         return false;

      // bChkAutoSubmit - Fix for google calendar loop https://www.progressiverobot.com/forum/viewtopic.php?f=7&t=24423
      if (bChkAutoSubmit && pMsgData->IsAutoSubmitted())
         return false;

      return true;
   }

}
