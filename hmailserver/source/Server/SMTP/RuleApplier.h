// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../Common/BO/RuleCriteria.h"

namespace HM
{

   class Account;
   class Message;
   class Rules;
   class Rule; 
   class RuleCriteria;
   class MessageData;
   class RuleAction;
   class RuleResult;
   
   class RuleApplier
   {
   public:
      RuleApplier(void);
      ~RuleApplier(void);

      void ApplyRules(std::shared_ptr<Rules> pRules, std::shared_ptr<const Account> account, std::shared_ptr<Message> pMessage, RuleResult &ruleResult);

      static bool TestMatch(String matchValue, RuleCriteria::MatchType matchType, String testValue);

      // Whether any active rule of the collection matches the message and has
      // a reject action - asked at the SMTP session before the message is
      // accepted, where the sender is still there to be told. text is the
      // action's, or the standard sentence.
      bool WouldReject(std::shared_ptr<Rules> pRules, std::shared_ptr<MessageData> pMsgData, String &text);

      // Whether the collection has an active rule with a reject action at all:
      // the cheap question, asked before the message is parsed for the real one.
      static bool HasRejectAction(std::shared_ptr<Rules> pRules);

      // Whether the rules a submission is asked against before it is queued -
      // the server's, and those of every domain whose rules act on the whole
      // message (DomainRuleScopes) - refuse the message. The SMTP session asks at the end of DATA and the
      // self-service send route asks before it queues, so a rule that blocks
      // speaks to this server's own webmail as it does to a mail client. The
      // collections are looked at first, in memory, and the message is parsed
      // for the match only when one of them has a reject action at all. text
      // is the action's, or the standard sentence naming the rule, one line.
      static bool RejectedBySubmissionRules(std::shared_ptr<Message> message, String &text);

      // A domain whose rules a message meets, and how far they reach. The
      // sending domain - the domain of the account that submitted it, and the
      // envelope sender's hosted domain where that is vouched for (the
      // submitter is its account, or DMARC passed for it) or where the message
      // reaches no other domain's mailboxes - acts on the whole message: it is
      // that domain's own mail. A domain the message is addressed to acts on
      // the whole message only when every recipient is its own; otherwise on
      // its own recipients alone (SetRecipientScope). Until 26 September 2026
      // every recipient domain's rules, and the envelope sender's domain's on
      // an unauthenticated claim, acted on the whole message, so one tenant's
      // delete, reject, header or route acted on another tenant's recipients.
      struct DomainRuleScope
      {
         __int64 domain_id;
         bool whole_message;
      };
      static std::vector<DomainRuleScope> DomainRuleScopes(std::shared_ptr<Message> message);

      // The rules to be applied are a recipient domain's that reach its own
      // recipients only (DomainRuleScopes). The actions that change the
      // message for everybody - a header, a disclaimer, a script function, a
      // route, a bound address, required TLS - are not taken; a copy goes to
      // the domain's recipients only; a delete, a reject or a quarantine is
      // left in the RuleResult for the deliverer, which takes the domain's
      // recipients off the message (a quarantine keeps a copy for them), and a
      // reject's notice to a hosted sender names those recipients alone and
      // says the others were not affected.
      void SetRecipientScope(__int64 domainId) { recipient_scope_domain_ = domainId; }

      static bool IsGeneratedResponseAllowed(std::shared_ptr<MessageData> pMsgData, bool bChkAutoSubmit);

   private:

      // Apply one rule to the message.
      bool ApplyRule_(std::shared_ptr<Rule> pRule, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData, bool &bContinueRuleProcessing, RuleResult &ruleResult);

      // Do the actions for the message.
      void ApplyActions_(std::shared_ptr<Rule> pRule, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData, bool &bContinueRuleProcessing, RuleResult &ruleResult);

      // Do the action
      void ApplyAction_(std::shared_ptr<Rule> pRule, std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData, bool &bContinueRuleProcessing, RuleResult &ruleResult);

      // True, with a debug line, when this pass repeats a delivery attempt that
      // already ran the rules (Message::FlagRepeatsAttempt): an action that
      // queues a new message - a forward, a copy, a reply - or whose effect
      // cannot be known, a script function, is then not taken again. Never for an account's
      // own rules (account set), which a held pass does not repeat.
      static bool RepeatsAttempt_(std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData, const String &action);

      // Check wether pMessage matches pCriteria.
      // sealedBodyDeclined is set when this criterion is a body criterion and the
      // message is signed or encrypted end to end. The criterion then does not
      // match, whatever the match type, because the body is unreadable rather
      // than empty - and the caller writes one line per rule saying so.
      bool MessageMatchesCriteria_(std::shared_ptr<RuleCriteria> pCriteria, std::shared_ptr<MessageData> pMsgData, bool &sealedBodyDeclined) const;
      
      // Actions
      void ApplyAction_Forward(std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const;
      void ApplyAction_Copy(std::shared_ptr<Rule> rule, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const;

      // Whether a sender domain is vouched for by what the server itself saw.
      static bool SenderDomainVouched_(std::shared_ptr<Message> message, __int64 senderDomainId, const String &senderDomainName);
      void ApplyAction_Reply(std::shared_ptr<Rule> pRule, std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const;
      void ApplyAction_ScriptFunction(std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const;
      void ApplyAction_AddDisclaimer(std::shared_ptr<Rule> pRule, std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const;

      // A notice to a hosted sender naming the rule, with the action's text.
      // Never to an outside sender (that is backscatter), never in answer to
      // an auto-submitted message, and under the rule loop limit.
      void ApplyAction_WarnSender(std::shared_ptr<Rule> pRule, std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const;

      // The notice itself, shared by warn_sender (beside whatever else the
      // rule does) and by a reject that met the message after acceptance,
      // where there was no session left to answer the sender - notSent says
      // which. A warning goes once per message: on the first delivery pass,
      // as DKIM signing does, and not again when the quarantine releases the
      // message, since its sender was told when it was held. A "not sent" has
      // no such guard: the message is gone, whichever pass that happened on.
      // Nor has an account's own rule (account set), which runs once for its
      // recipient, at local delivery, whichever pass delivers that recipient.
      void SendPolicyNotice_(std::shared_ptr<Rule> pRule, const String &actionText, std::shared_ptr<MessageData> pMsgData, bool notSent, std::shared_ptr<const Account> account) const;
      bool RuleMatches_(std::shared_ptr<Rule> pRule, std::shared_ptr<MessageData> pMsgData);
      void ApplyAction_SetHeader(std::shared_ptr<Rule> pRule, std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const;

      ;

   private:

      __int64 rule_account_id_;

      // See SetRecipientScope; 0 when the rules act on the whole message.
      __int64 recipient_scope_domain_;

      // True, with a debug line, when action would change the message for
      // recipients outside the scoped domain and so is not taken.
      bool OutsideScope_(const String &action) const;
      
   };
}