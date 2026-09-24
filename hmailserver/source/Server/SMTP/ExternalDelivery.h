// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DeliveryFailure.h"

#include <set>

namespace HM
{
   class Message;
   class MessageRecipient;
   class RuleResult;
   class ServerInfo;
   class HostNameAndIpAddress;

   class ExternalDelivery
   {
   public:
      ExternalDelivery(const String &sSendersIP, std::shared_ptr<Message> message, const RuleResult &globalRuleResult);
      ~ExternalDelivery(void);

      bool Perform(std::vector<DeliveryFailure> &saErrorMessages);

   private:



      void DeliverToSingleDomain_(std::vector<std::shared_ptr<MessageRecipient> > &batch, std::shared_ptr<ServerInfo> serverInfo);
      void DeliverToSingleServer_(std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients, std::shared_ptr<ServerInfo> serverInfo);

      bool ResolveRecipientServers_(std::shared_ptr<ServerInfo> &serverInfo, std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients, std::vector<HostNameAndIpAddress> &saMailServers);
      bool RecipientWithNonFatalDeliveryErrorExists_(std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients);
      // enhancedStatusCode has no default, for the reason DeliveryFailure's
      // constructor has no default: every one of these failures is a distinct
      // condition and the caller is the only place that knows which.
      void HandleExternalDeliveryFailure_(std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients, bool bIsFatal, String &sErrorString, const String &enhancedStatusCode);
      void HandleNoRecipientServers_(std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients, bool bDNSQueryOK, bool isSpecificRelayServer);

      // A deferral this server chose - a back-off window, a connection
      // ceiling, a per-minute rate - rather than one a remote inflicted. The
      // recipients are marked non-fatal with 4.4.5 and the reason, exactly as
      // a remote deferral marks them, and the pass remembers them so that
      // RescheduleDelivery_ can set the next try WITHOUT counting an attempt:
      // nothing was attempted, and a retry budget spent on this server's own
      // waiting is how a message is bounced that was never offered to anybody.
      void DeferBatchWithoutCounting_(std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients, int minutes, String &sErrorString);
      bool EveryFailureIsShaping_(const std::map<String,DeliveryFailure> &mapFailedDueToNonFatalError) const;

      // How long a deferral by a rate or a connection ceiling waits: one to
      // five minutes by the message's id, so that a backlog held behind one
      // ceiling does not come back as one pass per message every minute.
      int SpreadMinutes_() const;

      // Seconds since the message was accepted, from the creation time its row
      // carries; -1 when that cannot be read.
      long SecondsSinceCreation_() const;

      // The tenant of this message for the per-tenant connection share: the
      // hosted domain of the envelope sender (an alias domain resolved to its
      // domain), or "(external)" for mail this server relays for somebody else.
      String SenderTenant_() const;

      // Whether any recipient of the batch was answered, by a remote, with a
      // reply that asks this server to slow down (SMTP/DestinationBackoff.h).
      // Fills the reply and the host that gave it.
      static bool FindSlowDownReply_(const std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients, String &reply, String &host);

      // The pending failures are carried as DeliveryFailure rather than as the
      // address-to-prose map this used to be, so that the enhanced status code,
      // the remote server's reply and the host it came from survive the retry
      // window and are still there when the message is finally given up on.
      void CollectDeliveryResult_(const String &serverHostName, std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients, std::vector<DeliveryFailure> &saErrorMessages, std::map<String,DeliveryFailure> &mapFailedDueToNonFatalError, std::set<String> &suppressFailureDsnAddresses);
      bool RescheduleDelivery_(std::map<String,DeliveryFailure> &mapFailedDueToNonFatalError, std::vector<DeliveryFailure> &saErrorMessages, std::set<String> &suppressFailureDsnAddresses);
      // Type changed from void to bool for use with ETRN.
      // Function not called anywhere else to matter
      bool GetRetryOptions_(std::map<String,DeliveryFailure> &mapFailedDueToNonFatalError, long &lNoOfRetries, long &lMinutesBetween);

      // The source address for a connection to remote, or the unspecified
      // address when none is to be bound. logSkippedBind false leaves out the
      // line saying a bind of the other family was skipped - for the IP
      // warm-up's question before the connection, whose own asking says it.
      IPAddress GetLocalAddress_(const IPAddress &remote, bool logSkippedBind = true);

      const String &_sendersIP;
      const std::shared_ptr<Message> original_message_;
      const RuleResult &_globalRuleResult;   

      int quick_retries_;      
      int quick_retries_Minutes;      
      int queue_randomness_minutes_;
      int mxtries_factor_;

      // The recipients deferred by this server's own shaping in this pass, and
      // the longest of those deferrals in minutes. See DeferBatchWithoutCounting_.
      std::set<String> shaping_deferred_;
      int shaping_deferral_minutes_;

      // What the IP warm-up held in this pass (SMTP/IpWarmup.h): the
      // recipients, and what the log line and the report say about why.
      // DeliverToSingleDomain_ knows why and not when; RescheduleDelivery_
      // knows when, once it has chosen the path, and says both
      // (ReportWarmupHolds_).
      struct WarmupHold
      {
         WarmupHold() : day(0), ceiling(0), held_by_others(false) {}

         std::vector<String> addresses;   // the recipients held
         String address;                  // the source address in warm-up
         int day;                         // the warm-up day
         String plan;                     // the plan's name
         int ceiling;                     // the day's ceiling at the provider
         String provider;                 // the provider's name for a sentence
         bool held_by_others;             // the room is held by deliveries in flight, not sent
      };

      std::vector<WarmupHold> warmup_holds_;

      // The longest of this pass's warm-up holds, in minutes. A pass past its
      // retry span whose warm-up hold is longer than the retry interval is
      // counted at the hold's own length, not at the interval (see
      // RescheduleDelivery_); nothing else of the shaping row's is.
      int warmup_hold_minutes_;

      // "Delivery deferred by this server's IP warm-up: ..." - why, not when.
      static String WarmupHoldSentence_(const WarmupHold &hold);

      // When the next try is, from its minutes and whether it is counted:
      // after midnight UTC when that is where it lands, at the retry interval
      // when a counted try comes back inside the day.
      static String WarmupNextAttempt_(long minutes, bool counted);

      // Logs each hold with its next attempt and gives the held recipients'
      // pending failures the text a report would carry. Clears the holds.
      void ReportWarmupHolds_(std::map<String,DeliveryFailure> &mapFailedDueToNonFatalError, const String &nextAttempt);
   };
}