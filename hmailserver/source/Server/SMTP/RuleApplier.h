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
      // the server's, every hosted recipient domain's and the sender's domain's
      // - refuse the message. The SMTP session asks at the end of DATA and the
      // self-service send route asks before it queues, so a rule that blocks
      // speaks to this server's own webmail as it does to a mail client. The
      // collections are looked at first, in memory, and the message is parsed
      // for the match only when one of them has a reject action at all. text
      // is the action's, or the standard sentence naming the rule, one line.
      static bool RejectedBySubmissionRules(std::shared_ptr<Message> message, String &text);

      static bool IsGeneratedResponseAllowed(std::shared_ptr<MessageData> pMsgData, bool bChkAutoSubmit);

   private:

      // Apply one rule to the message.
      bool ApplyRule_(std::shared_ptr<Rule> pRule, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData, bool &bContinueRuleProcessing, RuleResult &ruleResult);

      // Do the actions for the message.
      void ApplyActions_(std::shared_ptr<Rule> pRule, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData, bool &bContinueRuleProcessing, RuleResult &ruleResult);

      // Do the action
      void ApplyAction_(std::shared_ptr<Rule> pRule, std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData, bool &bContinueRuleProcessing, RuleResult &ruleResult);

      // Check wether pMessage matches pCriteria.
      // sealedBodyDeclined is set when this criterion is a body criterion and the
      // message is signed or encrypted end to end. The criterion then does not
      // match, whatever the match type, because the body is unreadable rather
      // than empty - and the caller writes one line per rule saying so.
      bool MessageMatchesCriteria_(std::shared_ptr<RuleCriteria> pCriteria, std::shared_ptr<MessageData> pMsgData, bool &sealedBodyDeclined) const;
      
      // Actions
      void ApplyAction_Forward(std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const;
      void ApplyAction_Copy(std::shared_ptr<Rule> rule, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const;
      void ApplyAction_Reply(std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const;
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
      void SendPolicyNotice_(std::shared_ptr<Rule> pRule, const String &actionText, std::shared_ptr<MessageData> pMsgData, bool notSent) const;
      bool RuleMatches_(std::shared_ptr<Rule> pRule, std::shared_ptr<MessageData> pMsgData);
      void ApplyAction_SetHeader(std::shared_ptr<RuleAction> pAction, std::shared_ptr<const Account> account, std::shared_ptr<MessageData> pMsgData) const;

      ;

   private:

      __int64 rule_account_id_;
      
   };
}