// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DeliveryFailure.h"

namespace HM
{
   class Message;
   class MessageRecipient;
   class Account;
   class MessageData;
   class RuleResult;
   class ServerInfo;

   class SMTPDeliverer
   {
   public:
      SMTPDeliverer();
      virtual ~SMTPDeliverer();

      static void DeliverMessage(std::shared_ptr<Message> pMessage);

      // The non-delivery report, for a failure found after the message left the
      // delivery queue: an inbound webhook's endpoint that never accepted it
      // (Util/InboundWebhooks.h). originalMessage names the file the report
      // quotes the headers of, the sender it goes to and the id it is logged
      // under; the report is the one SubmitErrorLog_ builds for every failure.
      static void ReturnToSender(std::shared_ptr<Message> originalMessage, std::vector<DeliveryFailure> &failures);

   private:

      // deferDelivery distinguishes "abort this delivery" (the message is deleted,
      // which is what a false return has always meant) from "do not deliver this
      // now, but keep it" - a distinction that did not exist before the virus
      // scanner was given a fail-closed policy, and one worth stating plainly
      // because getting it backwards deletes mail.
      //
      // sandboxHeld is the second "keep it": the message is now held for a
      // sandbox's verdict on its attachments (messagetype 5, unlocked), and a
      // false return with it set must not delete anything either.
      static bool PreprocessMessage_(std::shared_ptr<Message> pMessage, RuleResult& globalRuleResult, String& preprocessingFailureReason, bool &deferDelivery, bool &sandboxHeld);

      static bool RunVirusProtection_(std::shared_ptr<Message> pMessage, bool &deferDelivery);

      // Attachments opened somewhere safe (Common/AntiVirus/Sandbox.h): after
      // the anti-virus scan, before the rules. False when the message is not to
      // be delivered now - held (sandboxHeld set), quarantined on a verdict, or
      // returned to its sender - and true otherwise, with any header written.
      static bool RunSandbox_(std::shared_ptr<Message> pMessage, bool &sandboxHeld);

      // Holds a message the scanner could not examine, or bounces it once the
      // hold budget is spent. Only reached when AVFailAction is 1.
      static void HandleUnscannableMessage_(std::shared_ptr<Message> pMessage);

      // Drops a message's AV hold count once it is past scanning, so the in-memory
      // table does not accumulate entries for messages that are long delivered.
      static void ForgetUnscannableHold_(__int64 messageID);
      static bool RunGlobalRules_(std::shared_ptr<Message> pMessage, RuleResult &ruleResult);

      // Builds and queues the non-delivery report. One DeliveryFailure per failed
      // recipient: the report is a multipart/report (RFC 3464) whose second part
      // is a per-recipient machine-readable record, and there is no way to
      // produce that from concatenated prose.
      static void SubmitErrorLog_(std::shared_ptr<Message> pOrigMessage, std::vector<DeliveryFailure> &saErrorMessages);

      static bool HandleInfectedMessage_(std::shared_ptr<Message> pMessage, const String &virusName);
      
      

      static void LogAwstatsMessageRejected_(const String &sSendersIP, std::shared_ptr<Message> pMessage, const String &sReason);

      static std::shared_ptr<Message> CreateAccountLevelMessage(std::shared_ptr<Message> pOriginalMessage, std::shared_ptr<Account> pRecipientAccount, bool reuseMessage, const String &sOriginalAddress);

      
   };
}