// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#include "StdAfx.h"

#include "./ExternalDelivery.h"

#include "RuleResult.h"

#include "../Common/BO/Message.h"
#include "../Common/BO/MessageRecipient.h"
#include "../Common/BO/MessageRecipients.h"
#include "../Common/BO/Routes.h"
#include "../Common/BO/RemoteDomainPolicies.h"
#include "../Common/BO/RemoteDomainPolicy.h"

#include "../Common/Scripting/Events.h"

#include "../Common/Persistence/PersistentMessageRecipient.h"
#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Persistence/MessageStore.h"

#include "../Common/TCPIP/DNSResolver.h"
#include "../Common/TCPIP/IOService.h"
#include "../Common/TCPIP/HostNameAndIpAddress.h"

#include "../Common/AntiSpam/DKIM/DKIM.h"
#include "../Common/AntiSpam/DKIM/DKIMSigner.h"

#include "../Common/Util/AWStats.h"
#include "../Common/Util/FileUtilities.h"
#include "../Common/Util/GatewayEncryption.h"
#include "../Common/Util/MessageTrace.h"
#include "../Common/Util/ServerInfo.h"
#include "../Common/Util/OutboundOAuth2TokenClient.h"
#include "../Common/Util/Parsing/StringParser.h"
#include "../Common/Application/IniFileSettings.h"
#include "../Common/Util/TlsRptStore.h"
#include "../Common/Util/RateLimiter.h"
#include "../Common/Util/RemoteDomainThrottle.h"
#include "../Common/Application/ObjectCache.h"
#include "../Common/BO/DomainAliases.h"
#include "../Common/Cache/CacheContainer.h"

#include "DestinationBackoff.h"
#include "IpWarmup.h"
#include "OutboundProviders.h"
#include "ServerTargetResolver.h"
#include "SMTPConfiguration.h"
#include "SMTPClientConnection.h"
#include "TlsPolicy.h"


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // Records a recipient whose delivery failed non-fatally, keyed by address.
      //
      // A plain mapFailed[address] = text is what this replaced, and it cannot
      // work any more: DeliveryFailure has no default constructor, precisely so
      // that no code path can produce a half-filled one. Find-then-insert or
      // assign is the same operation the subscript used to perform.
      void RememberNonFatalFailure_(std::map<String, DeliveryFailure> &pendingFailures,
                                    std::shared_ptr<MessageRecipient> recipient,
                                    const String &humanReadableText,
                                    const String &enhancedStatusCode)
      {
         DeliveryFailure failure(recipient->GetAddress(), enhancedStatusCode, humanReadableText);

         // Empty unless a remote server really answered - see MessageRecipient.
         failure.SetRemoteSmtpReply(recipient->GetRemoteSmtpReply());
         failure.SetRemoteMta(recipient->GetRemoteMta());

         auto existing = pendingFailures.find(recipient->GetAddress());

         if (existing == pendingFailures.end())
            pendingFailures.insert(std::make_pair(recipient->GetAddress(), failure));
         else
            existing->second = failure;
      }

      // Ends the probe of a destination whose back-off window has passed. On
      // the stack around the connection attempts, so that no path out of
      // DeliverToSingleDomain_ - an early return, a throw - can leave the
      // destination marked probing: a probe that was answered with a slow-down
      // reply has had its window doubled by then and this does nothing; any
      // other end of the probe opens the destination again.
      class BackoffProbe
      {
      public:
         BackoffProbe() : active_(false), slowed_down_(false) {}
         ~BackoffProbe()
         {
            if (active_ && !slowed_down_)
               DestinationBackoff::Instance()->ProbeEnded(target_);
         }

         void Begin(const String &target) { target_ = target; active_ = true; }
         void SlowedDown() { slowed_down_ = true; }
         bool IsActive() const { return active_; }

      private:
         BackoffProbe(const BackoffProbe &);
         BackoffProbe &operator=(const BackoffProbe &);

         String target_;
         bool active_;
         bool slowed_down_;
      };

      // What a delivery reserved of its source address's warm-up room for the
      // day (SMTP/IpWarmup.h), settled when the attempts are over: what was
      // delivered is added to the day's sent, and the whole reservation is let
      // go - so a recipient that was not delivered is not counted, and a
      // refused connection, a remote deferral or a refusal at RCPT TO leave the
      // day's count where it was. On the stack around the connection attempts,
      // as the back-off probe is, so that no path out of DeliverToSingleDomain_
      // keeps a reservation.
      class WarmupReservation
      {
      public:
         WarmupReservation() : active_(false), schedule_id_(0), day_(0), provider_(OutboundProviders::Other), reserved_(0) {}
         ~WarmupReservation()
         {
            if (!active_)
               return;

            try
            {
               int delivered = 0;
               for (std::shared_ptr<MessageRecipient> recipient : recipients_)
               {
                  if (recipient->GetDeliveryResult() == MessageRecipient::ResultOK)
                     delivered++;
               }

               IpWarmup::Instance()->Settle(schedule_id_, day_, provider_, reserved_, delivered);
            }
            catch (...)
            {
               // A destructor does not throw; what was reserved stays held in
               // memory until the next reinitialise.
            }
         }

         void Begin(__int64 scheduleId, __int64 day, OutboundProviders::Provider provider, int reserved,
                    const std::vector<std::shared_ptr<MessageRecipient> > &recipients)
         {
            schedule_id_ = scheduleId;
            day_ = day;
            provider_ = provider;
            reserved_ = reserved;
            recipients_ = recipients;
            active_ = true;
         }

      private:
         WarmupReservation(const WarmupReservation &);
         WarmupReservation &operator=(const WarmupReservation &);

         bool active_;
         __int64 schedule_id_;
         __int64 day_;
         OutboundProviders::Provider provider_;
         int reserved_;
         std::vector<std::shared_ptr<MessageRecipient> > recipients_;
      };

      // The encrypted copy of one batch's message, removed from the temporary
      // directory when the batch is done, whichever way DeliverToSingleDomain_
      // leaves: every one of its early returns, and a throw.
      class GatewayCopyFile
      {
      public:
         GatewayCopyFile() {}
         ~GatewayCopyFile()
         {
            if (!path_.IsEmpty())
               FileUtilities::DeleteFile(path_);
         }

         String &Path() { return path_; }

      private:
         GatewayCopyFile(const GatewayCopyFile &);
         GatewayCopyFile &operator=(const GatewayCopyFile &);

         String path_;
      };

      // One server's recipients, grouped by the gateway encryption that governs
      // them: those whose domain's policy encrypts to one partner key, or those
      // whose domain's policy encrypts nothing.
      struct EncryptionGroup
      {
         std::shared_ptr<RemoteDomainPolicy> encryption;
         std::vector<std::shared_ptr<MessageRecipient> > recipients;
      };

      // A batch is one message file on the wire, and gateway encryption rewrites
      // that file for a partner - so the recipients of a domain whose policy
      // encrypts must never share a batch with anybody else's, or the others
      // would be handed a message encrypted to a key that is not theirs. That
      // only arises where ServerTargetResolver has merged several domains onto
      // one relay or route target; an MX delivery is one domain already. The
      // groups keep the order the recipients came in. With no policy that
      // encrypts - every installation until an administrator writes one - there
      // is one group holding every recipient, and the batches are exactly the
      // ones there always were.
      std::vector<EncryptionGroup> GroupByEncryption_(const std::vector<std::shared_ptr<MessageRecipient> > &recipients)
      {
         std::vector<EncryptionGroup> groups;

         std::shared_ptr<RemoteDomainPolicies> policies =
            Configuration::Instance()->GetSMTPConfiguration()->GetRemoteDomainPolicies();

         for (std::shared_ptr<MessageRecipient> recipient : recipients)
         {
            std::shared_ptr<RemoteDomainPolicy> encryption;

            if (policies)
            {
               String domain = StringParser::ExtractDomain(recipient->GetAddress()).ToLower();
               std::shared_ptr<RemoteDomainPolicy> policy = policies->GetPolicyForDomain(domain);

               if (policy && policy->GetEncrypt() != RemoteEncryptNone)
                  encryption = policy;
            }

            const __int64 wanted = encryption ? encryption->GetID() : 0;

            EncryptionGroup *group = nullptr;
            for (EncryptionGroup &existing : groups)
            {
               const __int64 id = existing.encryption ? existing.encryption->GetID() : 0;
               if (id == wanted)
               {
                  group = &existing;
                  break;
               }
            }

            if (group == nullptr)
            {
               groups.push_back(EncryptionGroup());
               group = &groups.back();
               group->encryption = encryption;
            }

            group->recipients.push_back(recipient);
         }

         return groups;
      }
   }

   ExternalDelivery::ExternalDelivery(const String &sSendersIP, std::shared_ptr<Message> message, const RuleResult &globalRuleResult) :
      _sendersIP(sSendersIP),
      original_message_(message),
      _globalRuleResult(globalRuleResult),
      quick_retries_(0),
      quick_retries_Minutes(0),
      queue_randomness_minutes_(0),
      mxtries_factor_(0),
      shaping_deferral_minutes_(0),
      warmup_hold_minutes_(0)
   {

   }

   ExternalDelivery::~ExternalDelivery(void)
   {

   }

   /// Performs deliver to any external recipients. 
   /// Returns true if the message has been rescheduled for later delivery.
   bool
   ExternalDelivery::Perform(std::vector<DeliveryFailure> &saErrorMessages)
   {
      std::map<String,DeliveryFailure> mapFailedDueToNonFatalError;

      // DSN (RFC 3461): addresses whose recipients opted out of failure
      // notifications (NOTIFY=NEVER, or a NOTIFY list without FAILURE).
      std::set<String> suppressFailureDsnAddresses;

      ServerTargetResolver serverTargetResolver(original_message_, _globalRuleResult);
      std::map<std::shared_ptr<ServerInfo>, std::vector<std::shared_ptr<MessageRecipient> > > mapRecipients = serverTargetResolver.Resolve();
      auto iterDomain = mapRecipients.begin();
      auto iterEnd = mapRecipients.end();

      unsigned int iMaxRecipientsInBatch = Configuration::Instance()->GetSMTPConfiguration()->GetMaxSMTPRecipientsInBatch();
      if (iMaxRecipientsInBatch == 0)
         iMaxRecipientsInBatch = UINT_MAX;

      for (; iterDomain != iterEnd; iterDomain++)
      {
         std::shared_ptr<ServerInfo> serverInfo = (*iterDomain).first;

         // Grouped by the gateway encryption that governs them, so that a batch
         // is one partner's or nobody's. See GroupByEncryption_.
         const std::vector<EncryptionGroup> groups = GroupByEncryption_((*iterDomain).second);

         for (const EncryptionGroup &group : groups)
         {
            const std::vector<std::shared_ptr<MessageRecipient> > &vecRecipientsOnDomain = group.recipients;

            // Split up all the recipients for this server into batches of 100 or so.
            std::vector<std::shared_ptr<MessageRecipient> > batch;
            auto iterRecipient = vecRecipientsOnDomain.begin();
            while (iterRecipient != vecRecipientsOnDomain.end())
            {
               batch.push_back(*iterRecipient);

               if (batch.size() >= iMaxRecipientsInBatch ||
                  iterRecipient + 1 == vecRecipientsOnDomain.end())
               {
                  // Each batch gets its own copy: DeliverToSingleDomain_ overwrites the
                  // host name with the MX host it selects and can clear the connection
                  // security. Reusing one object meant every batch after the first
                  // looked up the MTA-STS policy of an MX host rather than of the
                  // recipient domain (so enforcement silently stopped applying), lost
                  // MX failover, and could inherit a STARTTLS downgrade from the
                  // previous batch.
                  std::shared_ptr<ServerInfo> batchServerInfo = std::shared_ptr<ServerInfo>(new ServerInfo(*serverInfo));

                  // Deliver the message to the remote server.
                  DeliverToSingleDomain_(batch, batchServerInfo, group.encryption);

                  // Check what status we got on the external deliveries.
                  CollectDeliveryResult_(batchServerInfo->GetHostName(), batch, saErrorMessages, mapFailedDueToNonFatalError, suppressFailureDsnAddresses);

                  batch.clear();
               }

               iterRecipient++;
            }
         }

      }

      if (mapFailedDueToNonFatalError.size() > 0)
      {   
         bool messageRescheduled = RescheduleDelivery_(mapFailedDueToNonFatalError, saErrorMessages, suppressFailureDsnAddresses);
         return messageRescheduled;
      }
      else
         return false;
   }

   void
   ExternalDelivery::DeliverToSingleDomain_(std::vector<std::shared_ptr<MessageRecipient> > &batch, std::shared_ptr<ServerInfo> serverInfo, std::shared_ptr<RemoteDomainPolicy> encryption)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Deliveres the message to external accounts (recipients not on this server).
   //---------------------------------------------------------------------------()
   {
      // The recipients this function delivers to: the batch's own pointers,
      // copied, so that the IP warm-up below can take the ones over the day's
      // ceiling out of the connection attempts while the caller's batch -
      // which CollectDeliveryResult_ reads afterwards - still holds every one.
      // A result set on a recipient here is set on the caller's too.
      std::vector<std::shared_ptr<MessageRecipient> > vecRecipients(batch);

      String sFirstRecipientAddress = vecRecipients[0]->GetAddress();
      if (sFirstRecipientAddress.IsEmpty())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 4226, "SMTPDeliverer::_DeliverToExternalAccounts", "Could not deliver message; Recipient address missing.");
         return;
      }

      std::vector<HostNameAndIpAddress> mail_servers;

      // The recipient domain (used for TLS policy lookups). For MX-based
      // deliveries the server info host name holds the domain name at this
      // point; it is replaced by individual MX host names further down.
      String recipientDomain = serverInfo->GetHostName();

      // The recipient domains this batch actually carries, which is NOT the same
      // question. For an MX delivery the server info's host name is the
      // recipient domain; for a route or a relay it is the target host - a name
      // belonging to whoever runs the smart host - and ServerTargetResolver
      // merges the recipients of several domains onto one such target. The
      // addresses are right in every one of those cases.
      std::vector<String> recipientDomains;
      for (std::shared_ptr<MessageRecipient> recipient : vecRecipients)
      {
         String domain = StringParser::ExtractDomain(recipient->GetAddress()).ToLower();

         if (domain.IsEmpty())
            continue;

         bool seen = false;
         for (const String &existing : recipientDomains)
         {
            if (existing == domain)
            {
               seen = true;
               break;
            }
         }

         if (!seen)
            recipientDomains.push_back(domain);
      }

      // What the administrator has said about these domains. Deliberately
      // applied to a routed delivery as well as to an MX one: the policy is a
      // statement about the DOMAIN, not about how its server was found, and a
      // route that quietly exempted a domain from the TLS its administrator
      // required would be the silent downgrade this whole record exists to
      // prevent. MTA-STS and DANE are the other way round - they are properties
      // the domain itself publishes about its MX hosts, and a route replaces MX
      // discovery, so they stay skipped for a fixed target.
      std::shared_ptr<RemoteDomainPolicies> policies =
         Configuration::Instance()->GetSMTPConfiguration()->GetRemoteDomainPolicies();
      std::shared_ptr<RemoteDomainPolicy> policy =
         policies ? policies->GetStrictestPolicy(recipientDomains) : std::shared_ptr<RemoteDomainPolicy>();

      RemoteTlsRequirement policyTls = policy ? policy->GetOutboundTlsRequirement() : RemoteTlsDefault;
      String policyName = policy ? policy->GetDomainName() : String();

      // The message that goes on the wire: the queued one, or - for a batch
      // whose domain's policy encrypts - the partner's copy, which is made
      // further down (Common/Util/GatewayEncryption.h). The composed record
      // above never carries the encryption - two policies cannot both encrypt
      // one message file - and it does not need to, because the batch was
      // grouped by it; what governs is the record that group was made for.
      std::shared_ptr<Message> batchMessage = original_message_;
      GatewayCopyFile gatewayCopy;

      // The largest message this server will offer to the remote.
      //
      // PERMANENT, unlike everything else this record can refuse: the message
      // will not be smaller on the next attempt, so retrying it for two days
      // before bouncing would delay the news without changing it. 5.3.4 is RFC
      // 3463's "message too big for system", which is what the remote would have
      // said itself had the administrator not already known it would.
      //
      // Measured here on the queued message, before anything is spent on it;
      // a partner's encrypted copy is larger still, and is measured again
      // once it is made, below.
      auto exceedsSizeCeiling = [&](__int64 messageSize) -> bool
      {
         if (!policy || policy->GetMaxMessageSizeKB() <= 0)
            return false;

         const __int64 sizeLimit = (__int64) policy->GetMaxMessageSizeKB() * 1024;
         if (messageSize <= sizeLimit)
            return false;

         LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Not attempted. The remote domain policy for " + policyName + " allows at most " + StringParser::IntToString((int) policy->GetMaxMessageSizeKB()) + " KB and this message is " + StringParser::IntToString((int) (messageSize / 1024)) + " KB.");

         String errorMessage;
         errorMessage.Format(_T("   Error Type: SMTP\r\n   Error Description: The message was not sent. The remote domain policy for %s allows at most %d KB to this destination and this message is %d KB.\r\n\r\n"),
            policyName.c_str(), (int) policy->GetMaxMessageSizeKB(), (int) (messageSize / 1024));

         HandleExternalDeliveryFailure_(vecRecipients, true, errorMessage, _T("5.3.4"));
         return true;
      };

      if (exceedsSizeCeiling(original_message_->GetSize()))
         return;

      // The destination, as the back-off and the connection ceilings name it:
      // the recipient domain of an MX delivery, the host of a route or a relay
      // - or its address, when the route names an IP literal, because
      // ServerTargetResolver puts a literal in the address and leaves the host
      // name empty, and an empty key would make every such route one
      // destination (which is what MaxOutboundPerDestinationPerMinute had
      // been doing since it was written: "smtp-out:" for all of them).
      String target = recipientDomain;
      if (target.IsEmpty())
         target = serverInfo->GetIpAddress();
      target.ToLower();

      // A destination that asked this server to slow down is not connected to
      // until its window has passed (SMTP/DestinationBackoff.h). Judged before
      // the rate and connection checks so that a held batch consumes no token
      // and no slot; after the size ceiling, because that one never connects
      // and its answer is permanent, and there is nothing to wait for.
      const int backoffMinutes = Configuration::Instance()->GetOutboundBackoffMinutes();
      const int backoffMaxMinutes = Configuration::Instance()->GetOutboundBackoffMaxMinutes();
      BackoffProbe probe;

      if (backoffMinutes > 0)
      {
         DestinationBackoff::Entry entry;
         int deferMinutes = 0;
         const time_t now = time(nullptr);

         const DestinationBackoff::Verdict verdict = DestinationBackoff::Instance()->Ask(target, now, entry, deferMinutes);

         if (verdict == DestinationBackoff::Wait)
         {
            const String since = DestinationBackoff::FormatTime(entry.since);
            const String nextAttempt = DestinationBackoff::FormatTime(now + (time_t) deferMinutes * 60);

            LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Delivery to " + target + " deferred: the destination asked this server to slow down at " + since + " (" + entry.reply + "); next attempt after " + nextAttempt + ".");

            String errorMessage;
            errorMessage.Format(_T("   Error Type: SMTP\r\n   Error Description: Delivery deferred: the destination %s asked this server to slow down at %s (%s). Next attempt after %s.\r\n\r\n"),
               target.c_str(), since.c_str(), entry.reply.c_str(), nextAttempt.c_str());

            DeferBatchWithoutCounting_(vecRecipients, deferMinutes, errorMessage);
            return;
         }

         if (verdict == DestinationBackoff::Probe)
         {
            probe.Begin(target);
            LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": The back-off window for " + target + " has passed; this delivery probes it.");
         }
      }

      // How many messages a minute this server will push at the remote. Its own
      // bucket, keyed by the policy rather than by the domain, so that one
      // pattern covering several domains is one ceiling - which is what an
      // administrator who wrote the pattern meant.
      if (policy && policy->GetMaxMessagesPerMinute() > 0)
      {
         String rateKey = _T("remote-policy:") + policyName;
         rateKey.ToLower();

         if (!RateLimiter::Instance()->TryConsume(rateKey, (int) policy->GetMaxMessagesPerMinute()))
         {
            LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Delivery deferred. The remote domain policy for " + policyName + " allows " + StringParser::IntToString((int) policy->GetMaxMessagesPerMinute()) + " message(s) a minute.");

            String errorMessage;
            errorMessage.Format(_T("   Error Type: SMTP\r\n   Error Description: Delivery deferred by the remote domain policy for %s, which allows %d message(s) a minute to this destination.\r\n\r\n"),
               policyName.c_str(), (int) policy->GetMaxMessagesPerMinute());

            // RFC 3463 X.4.5 "Mail system congestion", as the per-destination
            // rate limit below uses: the congestion is this server's own
            // deliberate shaping, and "too much mail for this route right now,
            // try later" is exactly what X.4.5 carries. One to five minutes,
            // spread by id; not counted, because nothing was attempted.
            DeferBatchWithoutCounting_(vecRecipients, SpreadMinutes_(), errorMessage);
            return;
         }
      }

      // How many sessions this server will hold open to the remote at once. The
      // slot lives to the end of this function, which is the end of the last
      // connection attempt for this batch; every return below releases it. A
      // policy's ceiling wins when there is one - an administrator who wrote it
      // keeps it - and the server-wide MaxOutboundConnectionsPerDestination
      // covers every destination no policy names.
      RemoteDomainThrottle::Slot connectionSlot;

      if (policy && policy->GetMaxConnections() > 0)
      {
         RemoteDomainThrottle::Instance()->Acquire(connectionSlot, policyName, (int) policy->GetMaxConnections());

         if (!connectionSlot.Acquired())
         {
            LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Delivery deferred. The remote domain policy for " + policyName + " allows " + StringParser::IntToString((int) policy->GetMaxConnections()) + " simultaneous connection(s), and they are all in use.");

            String errorMessage;
            errorMessage.Format(_T("   Error Type: SMTP\r\n   Error Description: Delivery deferred by the remote domain policy for %s, which allows %d simultaneous connection(s) to this destination.\r\n\r\n"),
               policyName.c_str(), (int) policy->GetMaxConnections());

            DeferBatchWithoutCounting_(vecRecipients, SpreadMinutes_(), errorMessage);
            return;
         }
      }
      else
      {
         const int maxPerDestination = Configuration::Instance()->GetMaxOutboundConnectionsPerDestination();

         if (maxPerDestination > 0)
         {
            RemoteDomainThrottle::Instance()->Acquire(connectionSlot, _T("destination:") + target, maxPerDestination);

            if (!connectionSlot.Acquired())
            {
               LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Delivery to " + target + " deferred. This server allows " + StringParser::IntToString(maxPerDestination) + " simultaneous connection(s) to one destination, and they are all in use.");

               String errorMessage;
               errorMessage.Format(_T("   Error Type: SMTP\r\n   Error Description: Delivery deferred: this server allows %d simultaneous connection(s) to %s, and they are all in use.\r\n\r\n"),
                  maxPerDestination, target.c_str());

               DeferBatchWithoutCounting_(vecRecipients, SpreadMinutes_(), errorMessage);
               return;
            }
         }
      }

      // How many deliveries one tenant may have in flight at once, so that one
      // hosted domain - or all the mail relayed for outsiders together - cannot
      // hold every delivery thread. A share of connections, bounded by the
      // delivery threads; the per-account sending limit is the volume control.
      // Released with the destination slot, at the end of this function.
      RemoteDomainThrottle::Slot tenantSlot;

      const int maxPerTenant = Configuration::Instance()->GetMaxOutboundConnectionsPerTenant();

      if (maxPerTenant > 0)
      {
         const String tenant = SenderTenant_();

         RemoteDomainThrottle::Instance()->Acquire(tenantSlot, _T("tenant:") + tenant, maxPerTenant);

         if (!tenantSlot.Acquired())
         {
            LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Delivery to " + target + " deferred. The tenant " + tenant + " may have " + StringParser::IntToString(maxPerTenant) + " deliver(y/ies) in flight at once, and they are all in use.");

            String errorMessage;
            errorMessage.Format(_T("   Error Type: SMTP\r\n   Error Description: Delivery deferred: the sending domain %s may have %d deliver(y/ies) in flight at once on this server, and they are all in use.\r\n\r\n"),
               tenant.c_str(), maxPerTenant);

            DeferBatchWithoutCounting_(vecRecipients, SpreadMinutes_(), errorMessage);
            return;
         }
      }

      // Per-destination outbound rate shaping. A configured [Settings]
      // MaxOutboundPerDestinationPerMinute caps how many messages this server
      // sends to a single destination per minute; when exceeded the delivery is
      // deferred (non-fatal) and retried later. 0 = unlimited (default, no-op).
      int maxOutboundPerDestination = IniFileSettings::Instance()->GetMaxOutboundPerDestinationPerMinute();
      if (maxOutboundPerDestination > 0)
      {
         if (!RateLimiter::Instance()->TryConsume(_T("smtp-out:") + target, maxOutboundPerDestination))
         {
            LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Delivery to " + target + " deferred due to the per-destination rate limit.");
            String errorMessage = _T("   Error Type: SMTP\r\n   Error Description: Delivery temporarily deferred by the sending server's per-destination rate limit.\r\n\r\n");
            // RFC 3463 X.4.5 "Mail system congestion". The congestion is this
            // server's own deliberate shaping rather than an overload, but the
            // meaning a sender needs - too much mail for this route right now,
            // try later - is exactly what X.4.5 carries. One to five minutes,
            // spread by id; not counted, because nothing was attempted.
            DeferBatchWithoutCounting_(vecRecipients, SpreadMinutes_(), errorMessage);
            return;
         }
      }

      // Run DNS query to find the recipient servers IP addresses.
      if (!ResolveRecipientServers_(serverInfo, vecRecipients, mail_servers))
         return;

      // Apply the recipient domain's MTA-STS policy (RFC 8461).
      bool stsEnforced = false;
      if (!serverInfo->GetFixed() && IniFileSettings::Instance()->GetMtaStsEnabled())
      {
         TlsPolicy::StsPolicy stsPolicy = TlsPolicy::GetStsPolicy(recipientDomain);

         if (stsPolicy.mode == TlsPolicy::StsEnforce)
         {
            std::vector<HostNameAndIpAddress> matchingServers;
            for (const HostNameAndIpAddress &mailServer : mail_servers)
            {
               HostNameAndIpAddress serverCopy = mailServer;
               if (TlsPolicy::HostMatchesStsPolicy(serverCopy.GetHostName(), stsPolicy))
                  matchingServers.push_back(mailServer);
               else
               {
                  LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Skipping MX host " + serverCopy.GetHostName() + " - not permitted by the MTA-STS policy of " + recipientDomain + ".");
                  TlsRptStore::Instance()->RecordFailure(recipientDomain, "sts", "mode: enforce", "validation-failure", serverCopy.GetHostName());
               }
            }

            if (matchingServers.empty())
            {
               LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Delivery to " + recipientDomain + " deferred. No MX host matches the domain's MTA-STS policy.");

               String errorMessage = _T("   Error Type: SMTP\r\n   Error Description: Delivery blocked by the recipient domain's MTA-STS policy. None of the MX hosts match the published policy.\r\n\r\n");
               // RFC 3463 X.7.0 "Other or undefined security status". RFC 8461
               // registers no enhanced code of its own, and the undefined
               // security code says the true thing - refused on security grounds
               // - without borrowing a code that means something else.
               HandleExternalDeliveryFailure_(vecRecipients, false, errorMessage, _T("4.7.0"));
               return;
            }

            mail_servers = matchingServers;
            stsEnforced = true;

            LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": MTA-STS policy mode 'enforce' active for " + recipientDomain + ". TLS with certificate validation is required.");
         }
         else if (stsPolicy.mode == TlsPolicy::StsTesting)
         {
            LOG_DEBUG("MTA-STS: Domain " + recipientDomain + " publishes a policy in 'testing' mode. Not enforcing.");
         }
      }

      // The IP warm-up (SMTP/IpWarmup.h, docs/IpWarmup.md). Judged here, once
      // the hosts are known and the source address the first of them will be
      // connected from is known, since that address is what a schedule is
      // about; the provider is the one that first host belongs to, never the
      // recipient's domain's, so a company on Google Workspace is Google, and
      // never a backup host's further down the list. A delivery with no bind
      // address, from an address with no enabled schedule, or to a provider
      // with no ceiling today is untouched. Otherwise the batch's recipients
      // are reserved of today's room up to the ceiling before any connection,
      // and the ones over it are held as every shaping deferral is, counted as
      // deferred: to the first minutes of the next UTC day when the day's
      // ceiling is what was reached, a few minutes when the room is held by
      // other deliveries in flight that may give it back. When the attempts
      // are over the reservation is settled (WarmupReservation): what was
      // delivered is sent, the rest is let go. When and whether the held ones
      // are tried again is RescheduleDelivery_'s to say (ReportWarmupHolds_).
      WarmupReservation warmupReservation;

      // The schedule the batch was judged against, or 0: a later host that
      // would be reached from another scheduled address is not tried for it.
      __int64 warmupScheduleJudged = 0;

      {
         HostNameAndIpAddress firstHost = mail_servers[0];

         IPAddress firstRemote;
         firstRemote.TryParse(AnsiString(firstHost.GetIpAddress()), false);

         const IPAddress bindAddress = GetLocalAddress_(firstRemote, false);
         IpWarmup::Schedule schedule;

         if (!bindAddress.IsAny() && IpWarmup::Instance()->FindEnabled(String(bindAddress.ToString()), schedule))
         {
            warmupScheduleJudged = schedule.id;

            const OutboundProviders::Provider provider = OutboundProviders::ClassifyHost(firstHost.GetHostName());
            const __int64 now = (__int64) time(nullptr);
            const __int64 today = IpWarmup::DayOf(now);
            const int warmupDay = IpWarmup::WarmupDay(schedule.started, today);
            const int ceiling = IpWarmup::Ceiling(schedule.plan, warmupDay, provider);

            if (ceiling >= 0)
            {
               const int wanted = (int) vecRecipients.size();
               int sentBefore = 0;
               bool heldByOthers = false;

               // -1 when the count could not be read or started: reported, and
               // the delivery goes as though there were no schedule.
               const int reserved = IpWarmup::Instance()->Reserve(schedule.id, today, provider, wanted, ceiling, warmupDay, sentBefore, heldByOthers);

               if (reserved >= 0 && reserved < wanted)
               {
                  std::vector<std::shared_ptr<MessageRecipient> > overflow(vecRecipients.begin() + reserved, vecRecipients.end());
                  vecRecipients.erase(vecRecipients.begin() + reserved, vecRecipients.end());

                  IpWarmup::Instance()->AddDeferred(schedule.id, today, provider, (int) overflow.size());

                  // The day's ceiling reached: until the first minutes of the
                  // next UTC day, when the count starts again, spread by the
                  // message's id as the other shaping deferrals are, so a held
                  // backlog does not come back as one pass in the same minute.
                  // Room held by deliveries in flight: those minutes alone,
                  // since they may give it back long before midnight.
                  const int untilMidnight = (int) (((today + 1) * IpWarmup::DaySeconds - now + 59) / 60);
                  const int deferMinutes = heldByOthers ? SpreadMinutes_() : untilMidnight + SpreadMinutes_();

                  WarmupHold hold;
                  hold.address = String(bindAddress.ToString());
                  hold.day = warmupDay;
                  hold.plan = IpWarmup::PlanName(schedule.plan);
                  hold.ceiling = ceiling;
                  hold.provider = OutboundProviders::DisplayName(provider);
                  hold.held_by_others = heldByOthers;

                  for (std::shared_ptr<MessageRecipient> recipient : overflow)
                     hold.addresses.push_back(recipient->GetAddress());

                  // Why, and not when: RescheduleDelivery_ adds the next attempt
                  // once it knows which path the message takes.
                  String errorMessage = _T("   Error Type: SMTP\r\n   Error Description: ") + WarmupHoldSentence_(hold) + _T("\r\n\r\n");

                  warmup_holds_.push_back(hold);

                  if (deferMinutes > warmup_hold_minutes_)
                     warmup_hold_minutes_ = deferMinutes;

                  DeferBatchWithoutCounting_(overflow, deferMinutes, errorMessage);

                  if (vecRecipients.empty())
                     return;
               }

               if (reserved > 0)
                  warmupReservation.Begin(schedule.id, today, provider, reserved, vecRecipients);
            }
         }
      }

      // A policy that asks for DANE asks for the TLSA lookup whatever the global
      // switch says and whatever kind of target this is: the administrator has
      // named the proof they want, and answering "DANE is off in the ini" by
      // delivering in the clear is the silent downgrade. The lookup itself is
      // unchanged, and so is every outcome of it - Insecure and NoRecords still
      // mean "deliver without DANE" for every OTHER delivery; it is only the
      // policy's own requirement below that turns "no usable record" into a
      // refusal, and only for the domains the policy names.
      bool daneEnabled = (!serverInfo->GetFixed() && IniFileSettings::Instance()->GetDaneEnabled()) ||
                         policyTls == RemoteTlsDane;

      // RFC 7672 section 2.2: DANE applies only to a host learned from a
      // DNSSEC-validated MX RRset. Validating the TLSA record alone proves nothing
      // when the attacker chose the host - a forged MX answer points at a name whose
      // own TLSA record then validates perfectly, and DANE reports success.
      //
      // Looked up once per domain rather than per host, and only when DANE is on
      // with DNSSEC validation (without it the legacy mode takes TLSA records as
      // published, and nothing here is consulted). An unsigned domain - which is
      // most of them - is Insecure once the resolver has proven the delegation
      // unsigned, and Insecure means "deliver without DANE". Bogus, Indeterminate
      // (RFC 7672 section 2.1.1: the lookup did not complete, or a signed zone's
      // answer came without its signatures or its proof) and a host the Secure
      // RRset does not name (section 2.2.1) each leave the host unused below.
      DnssecResolver::ChainStatus mxChainStatus = DnssecResolver::ChainStatus::Insecure;
      std::vector<String> validatedMxHosts;

      // The resolver's own words for a Bogus or Indeterminate verdict - which
      // RRset, which key tag, which check (DnssecResolver::GetFailureReason) - in
      // brackets after the application log line that reports it, so that the
      // line says why and not only that.
      String mxFailureReason;
      auto because = [](const String &reason) -> String
      {
         return reason.IsEmpty() ? String() : String(_T(" (")) + reason + _T(")");
      };

      if (daneEnabled && IniFileSettings::Instance()->GetDnssecValidationEnabled())
      {
         std::vector<AnsiString> exchanges;
         DnssecResolver dnssecResolver;
         mxChainStatus = dnssecResolver.QueryMx(recipientDomain, exchanges);
         mxFailureReason = dnssecResolver.GetFailureReason();

         for (const AnsiString &exchange : exchanges)
            validatedMxHosts.push_back(String(exchange));

         if (mxChainStatus == DnssecResolver::ChainStatus::Secure)
            LOG_DEBUG("DANE: the MX RRset for " + recipientDomain + " is DNSSEC-validated; DANE may be applied to the " + StringParser::IntToString((int) validatedMxHosts.size()) + " host(s) it names.");
      }

      // Gateway encryption (Common/Util/GatewayEncryption.h): the partner's
      // copy is made HERE, after every decision that could hold the batch
      // back without connecting - the back-off window, the per-minute rate,
      // the connection and tenant ceilings, the per-destination rate, the MX
      // lookup, MTA-STS, the IP warm-up's ceiling for the day (which takes the
      // recipients over it out of vecRecipients first, so the copy is made for
      // the ones that go) - so that a batch that is only going to be deferred is
      // never read, encrypted, signed and thrown away, and the log line and
      // the trace's "encrypted" event mean a copy that is about to be offered.
      // Before the connection attempts, because every one of them sends it.
      if (encryption)
      {
         if (!EncryptBatch_(vecRecipients, encryption, batchMessage, gatewayCopy.Path()))
            return;

         // The copy is what goes on the wire, so it is what the ceiling
         // measures too: armour and base64 make it about a third larger.
         if (batchMessage != original_message_ && exceedsSizeCeiling(batchMessage->GetSize()))
            return;
      }

      mxtries_factor_ = IniFileSettings::Instance()->GetMXTriesFactor();

      // Try to connect to one server at a time. If a fatal error
      // occurs, (an exception with eFatalError), we should stop trying
      // and just return an error message.

      unsigned int attemptedHosts = 0;

      // Why every host was skipped, when every host was. Written by whichever
      // rule did the skipping so that the deferral names it: a refusal that does
      // not say which rule refused is a refusal an administrator cannot act on.
      String skippedReason = _T("the DANE TLSA records of all MX hosts failed DNSSEC validation");
      String skippedDescription = _T("Delivery blocked: the DANE TLSA records of all MX hosts failed DNSSEC validation.");
      const String messageIdText = StringParser::IntToString(original_message_->GetID());

      for (unsigned int i = 0; i < mail_servers.size(); i++)
      {
         HostNameAndIpAddress hostAndIp = mail_servers[i];

         // Create a list of the remaining recipients. These are the recipients we have
         // not yet delivered to on a previous server (where i > 0). 
         std::vector<std::shared_ptr<MessageRecipient> > remainingRecipients;
         for(std::shared_ptr<MessageRecipient> recipient : vecRecipients)
         {
            if (recipient->GetDeliveryResult() == MessageRecipient::ResultUndefined ||
                recipient->GetDeliveryResult() == MessageRecipient::ResultNonFatalError)
            {
               remainingRecipients.push_back(recipient);
            }
         }

         serverInfo->SetHostName(hostAndIp.GetHostName());
         serverInfo->SetIpAddress(hostAndIp.GetIpAddress());

         // Apply per-host TLS requirements: MTA-STS enforcement applies to
         // all hosts; DANE pins are looked up per MX host (RFC 7672); a remote
         // domain policy applies to every host of the domains it names.
         //
         // OR, never assignment. The three sources of a TLS requirement compose
         // as the strongest of them, and this is the line where that is true or
         // not: a domain that publishes an MTA-STS enforce policy AND is named
         // here at "encrypted" still gets certificate verification, because
         // MTA-STS asked for it and nothing in this record can take it away.
         serverInfo->SetRequirePeerVerification(stsEnforced || policyTls >= RemoteTlsVerified);

         std::vector<TlsaRecord> daneRecords;
         bool daneValidated = false;

         // RFC 7672 section 2.2: DNSSEC-validated TLSA records of which none is
         // usable. TLS is required to the host, and its certificate is not
         // judged by DANE - there is nothing to judge it with.
         bool daneTlsRequired = false;

         if (daneEnabled && !hostAndIp.GetHostName().IsEmpty())
         {
            // The RFC 7672 section 2.2 status of THIS host: the MX RRset validated,
            // and this host is one of the names it published.
            TlsPolicy::MxDnssecStatus mxStatus =
               TlsPolicy::EvaluateMxDnssecStatus(hostAndIp.GetHostName(), mxChainStatus, validatedMxHosts);

            if (mxStatus == TlsPolicy::MxDnssecStatus::Bogus)
            {
               if (mxChainStatus == DnssecResolver::ChainStatus::Indeterminate)
               {
                  // RFC 7672 section 2.1.1: the validated MX lookup did not
                  // complete, or the domain's signed zone answered without its
                  // signatures or its proof. Not reported to TLS reporting: the
                  // domain may not be signed at all, which is what could not be told.
                  LOG_APPLICATION("SMTPDeliverer - Message " + messageIdText + ": the DNSSEC lookup of the MX RRset for " + recipientDomain + " could not be completed, or came without the signatures or the proof its zone owes" + because(mxFailureReason) + ". Skipping " + hostAndIp.GetHostName() + " (RFC 7672 section 2.1.1).");
                  skippedReason = _T("the DNSSEC lookup of the MX records of ") + recipientDomain + _T(" could not be completed, so whether DANE applies could not be told");
                  skippedDescription = _T("Delivery deferred: the DNSSEC lookup of the MX records of ") + recipientDomain +
                     _T(" could not be completed, or came without the signatures or the proof its zone publishes, so whether the domain requires DANE (RFC 7672) could not be told. The message has NOT been sent; it will be retried.");
               }
               else if (mxChainStatus == DnssecResolver::ChainStatus::Secure)
               {
                  // The host came from the plain, unvalidated lookup, and the
                  // validated RRset does not name it (RFC 7672 section 2.2.1).
                  LOG_APPLICATION("SMTPDeliverer - Message " + messageIdText + ": " + hostAndIp.GetHostName() + " is not named by the DNSSEC-validated MX RRset of " + recipientDomain + ". Skipping this host (RFC 7672 section 2.2.1).");
                  skippedReason = _T("no mail host found for ") + recipientDomain + _T(" is one its DNSSEC-validated MX records name");
                  skippedDescription = _T("Delivery deferred: none of the mail hosts found for ") + recipientDomain +
                     _T(" is one its DNSSEC-validated MX records name. The message has NOT been sent; it will be retried.");
               }
               else
               {
                  // A forged or broken chain over the MX RRset itself. The host name is
                  // not trustworthy, so neither is anything published under it.
                  LOG_APPLICATION("SMTPDeliverer - Message " + messageIdText + ": the MX RRset for " + recipientDomain + " failed DNSSEC validation" + because(mxFailureReason) + ". Skipping " + hostAndIp.GetHostName() + " (RFC 7672 section 2.2).");
                  TlsRptStore::Instance()->RecordFailure(recipientDomain, "tlsa", "mx rrset dnssec validation", "dnssec-invalid", hostAndIp.GetHostName());
                  skippedReason = _T("the MX records of ") + recipientDomain + _T(" failed DNSSEC validation");
                  skippedDescription = _T("Delivery blocked: the MX records of ") + recipientDomain + _T(" failed DNSSEC validation.");
               }
               continue;
            }

            TlsPolicy::TlsaLookupStatus tlsaStatus = TlsPolicy::TlsaLookupStatus::NoRecords;
            String tlsaFailureReason;
            daneRecords = TlsPolicy::GetTlsaRecords(hostAndIp.GetHostName(), serverInfo->GetPort(), tlsaStatus, mxStatus, &tlsaFailureReason);

            if (tlsaStatus == TlsPolicy::TlsaLookupStatus::Bogus)
            {
               // RFC 7672 section 2.1.3: a bogus DNSSEC chain means the
               // host must not be used for delivery.
               LOG_APPLICATION("SMTPDeliverer - Message " + messageIdText + ": TLSA records for " + hostAndIp.GetHostName() + " failed DNSSEC validation" + because(tlsaFailureReason) + ". Skipping this host (RFC 7672).");
               TlsRptStore::Instance()->RecordFailure(recipientDomain, "tlsa", "dane-ee tlsa records present", "dnssec-invalid", hostAndIp.GetHostName());
               skippedReason = _T("the DANE TLSA records of all MX hosts failed DNSSEC validation");
               skippedDescription = _T("Delivery blocked: the DANE TLSA records of all MX hosts failed DNSSEC validation.");
               continue;
            }

            if (tlsaStatus == TlsPolicy::TlsaLookupStatus::Indeterminate)
            {
               // RFC 7672 section 2.1.1: the host's TLSA lookup failed, or its
               // signed zone answered without the signatures or the proof it owes.
               // Until 6.3.6 the host was delivered to as if it published none.
               LOG_APPLICATION("SMTPDeliverer - Message " + messageIdText + ": the DNSSEC lookup of the TLSA records of " + hostAndIp.GetHostName() + " could not be completed, or came without the signatures or the proof its zone owes" + because(tlsaFailureReason) + ". Skipping this host (RFC 7672 section 2.1.1).");
               TlsRptStore::Instance()->RecordFailure(recipientDomain, "tlsa", "tlsa records could not be looked up", "dnssec-invalid", hostAndIp.GetHostName());
               skippedReason = _T("the DNSSEC lookup of the TLSA records of the mail hosts of ") + recipientDomain + _T(" could not be completed");
               skippedDescription = _T("Delivery deferred: the DNSSEC lookup of the TLSA records of the mail hosts of ") + recipientDomain +
                  _T(" could not be completed, or came without the signatures or the proof their zone publishes, so whether they require DANE (RFC 7672) could not be told. The message has NOT been sent; it will be retried.");
               continue;
            }

            daneValidated = tlsaStatus == TlsPolicy::TlsaLookupStatus::DnssecValidated;
            daneTlsRequired = tlsaStatus == TlsPolicy::TlsaLookupStatus::Unusable;

            if (!daneRecords.empty())
            {
               if (daneValidated)
               {
                  LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": DNSSEC-validated DANE TLSA records found for " + hostAndIp.GetHostName() + ". TLS with TLSA certificate matching is required.");
               }
               else
               {
                  LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": DANE TLSA records found for " + hostAndIp.GetHostName() + " (unvalidated). TLS with TLSA certificate matching is required.");
               }
            }
            else if (daneTlsRequired)
            {
               // Until 6.3.6 this was delivery as if no record were published:
               // opportunistic, and in the clear when the STARTTLS offer was
               // taken away on the way.
               LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": DNSSEC-validated TLSA records found for " + hostAndIp.GetHostName() + ", none of them usable. TLS is required, without certificate matching (RFC 7672 section 2.2).");
            }
         }

         // A policy that asked for DANE and got a host with no usable
         // DNSSEC-validated TLSA record. There is nothing to verify the
         // certificate against, so there is no way to deliver to this host that
         // satisfies what was asked for - which is the point of asking for it.
         // The host is skipped like a bogus one; if none is left the delivery is
         // deferred below, naming the rule.
         if (policyTls == RemoteTlsDane &&
             (daneRecords.empty() || !daneValidated))
         {
            LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Skipping " + hostAndIp.GetHostName() + " - the remote domain policy for " + policyName + " requires DANE and this host publishes no DNSSEC-validated TLSA record.");

            skippedReason = _T("the remote domain policy for ") + policyName + _T(" requires DANE");
            skippedDescription = _T("Delivery blocked by the remote domain policy for ") + policyName +
               _T(", which requires DANE. No mail host for this domain publishes a DNSSEC-validated TLSA record.");

            TlsRptStore::Instance()->RecordFailure(recipientDomain, "tlsa", "dane required by local policy", "validation-failure", hostAndIp.GetHostName());
            continue;
         }

         // The IP warm-up, for this host: the batch was judged against the
         // address its first host is reached from, and a host of the other
         // family is reached from whatever address the settings give that
         // family. When that is another address in warm-up - or one in warm-up
         // when the first host's address had none - this batch has reserved
         // nothing of its day, and delivering from it would be uncounted and
         // past its ceiling. The host is skipped, and the next one tried.
         {
            IPAddress hostRemote;
            hostRemote.TryParse(AnsiString(hostAndIp.GetIpAddress()), false);

            const IPAddress hostLocal = GetLocalAddress_(hostRemote, false);
            IpWarmup::Schedule hostSchedule;

            if (!hostLocal.IsAny() &&
                IpWarmup::Instance()->FindEnabled(String(hostLocal.ToString()), hostSchedule) &&
                hostSchedule.id != warmupScheduleJudged)
            {
               const int hostCeiling = IpWarmup::Ceiling(hostSchedule.plan,
                  IpWarmup::WarmupDay(hostSchedule.started, IpWarmup::DayOf((__int64) time(nullptr))),
                  OutboundProviders::ClassifyHost(hostAndIp.GetHostName()));

               if (hostCeiling >= 0)
               {
                  LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Skipping " + hostAndIp.GetHostName() + " (" + hostAndIp.GetIpAddress() + "): it would be reached from " + hostSchedule.address + ", an address in IP warm-up that this batch was not counted against.");

                  skippedReason = _T("every host left would be reached from an address in IP warm-up that the batch was not counted against");
                  skippedDescription = _T("Delivery deferred: every mail host left would be reached from an address in IP warm-up that this delivery was not counted against.");
                  continue;
               }
            }
         }

         attemptedHosts++;

         serverInfo->SetDaneRecords(daneRecords);
         serverInfo->SetRequireTls(stsEnforced || !daneRecords.empty() || daneTlsRequired || policyTls >= RemoteTlsEncrypted || _globalRuleResult.GetRequireTls());

         // The certificate is forgiven as on opportunistic STARTTLS when the
         // unusable TLSA records are all that require TLS: they ask for TLS and
         // not for authentication. Anything else that requires TLS keeps the
         // verification it has always had - this server's own connection
         // security at SSL or STARTTLS (required) included, which 6.3.6 before
         // its release did not ask (TlsPolicy::WaivesCertificateCheck).
         serverInfo->SetAcceptUnauthenticatedTls(TlsPolicy::WaivesCertificateCheck(daneTlsRequired, stsEnforced,
            policyTls >= RemoteTlsEncrypted, _globalRuleResult.GetRequireTls(), serverInfo->GetConnectionSecurity()));

         // The sentence a deferral carries when the encryption could not be had:
         // a remote domain policy's, a rule's, or - what the destination
         // published - its TLSA records' or its MTA-STS policy's
         // (TlsPolicy::PublishedTlsDeferral). Empty only when nothing but this
         // server's own connection security asked for TLS, whose failure is the
         // permanent 5.7.0 a misconfigured relay has always had. Until 6.3.6 a
         // host with usable TLSA records, or under an MTA-STS policy in enforce
         // mode, that did not offer STARTTLS was bounced at once with that 5.7.0,
         // while RFC 7672 section 2.2 and RFC 8461 section 5 both ask for the
         // delivery to be retried - as it was for TLSA records none of which is
         // usable.
         if (policyTls >= RemoteTlsEncrypted)
         {
            String requirement;

            switch (policyTls)
            {
            case RemoteTlsDane:
               requirement = _T("TLS proven by a DNSSEC-validated TLSA record");
               break;
            case RemoteTlsVerified:
               requirement = _T("TLS with a certificate that verifies");
               break;
            default:
               requirement = _T("TLS");
               break;
            }

            // One plain sentence, as every other reason SMTPClientConnection
            // records is: it wraps the sentence in its own "Error Type" lines,
            // and a pre-wrapped block would appear inside them a second time.
            String reason;
            reason.Format(_T("Delivery deferred: the remote domain policy for %s requires %s to this destination, and it could not be established. The message has NOT been sent in the clear."),
               policyName.c_str(), requirement.c_str());

            serverInfo->SetTlsPolicyReason(reason);
         }
         else if (_globalRuleResult.GetRequireTls())
         {
            // A rule required it: a deferral naming the rule, as a remote
            // domain policy's names the policy.
            String reason;
            reason.Format(_T("Delivery deferred: the rule %s requires TLS to this destination, and it could not be established. The message has NOT been sent; it will be retried."),
               _globalRuleResult.GetRequireTlsRuleName().c_str());
            serverInfo->SetTlsPolicyReason(reason);
         }
         else
         {
            // A deferral, as a policy's is: the host may offer STARTTLS again
            // before the queue gives up, and what took the offer away may have
            // been on the way rather than at the host.
            serverInfo->SetTlsPolicyReason(TlsPolicy::PublishedTlsDeferral(hostAndIp.GetHostName(), recipientDomain,
               !daneRecords.empty(), daneTlsRequired, stsEnforced));
         }

         // Classification for TLS reporting (RFC 8460).
         AnsiString rptPolicyType = !daneRecords.empty() || daneTlsRequired ? "tlsa" : (stsEnforced ? "sts" : "no-policy-found");
         AnsiString rptPolicyString = !daneRecords.empty() ? "dane tlsa records present" :
            (daneTlsRequired ? "tlsa records present, none usable: tls required" : (stsEnforced ? "mode: enforce" : ""));

         const SmtpTlsResult tlsResult = DeliverToSingleServer_(remainingRecipients, serverInfo, batchMessage);

         bool retryWithoutStartTls = false;

         for(std::shared_ptr<MessageRecipient> recipient : remainingRecipients)
         {
            if (recipient->GetDeliveryResult() == MessageRecipient::ResultOptionalHandshakeFailed)
            {
               recipient->SetDeliveryResult(MessageRecipient::ResultUndefined);
               retryWithoutStartTls = true;
            }
         }

         // A session is reported as what became of its TLS (RFC 8460 section
         // 4.3): a success only when the handshake completed. Until 6.3.6 every
         // session whose security asked for TLS was counted a success - a host
         // that offered no STARTTLS, a certificate that did not match its TLSA
         // record or verify for MTA-STS, a connection that never got as far - so
         // a domain's report hid the very failures it is published to learn of.
         // A session that ended before TLS was asked for is not one.
         if (!serverInfo->GetFixed())
         {
            if (tlsResult == SmtpTlsResult::Established)
               TlsRptStore::Instance()->RecordSuccess(recipientDomain, rptPolicyType, rptPolicyString);
            else if (tlsResult == SmtpTlsResult::StartTlsNotOffered)
               TlsRptStore::Instance()->RecordFailure(recipientDomain, rptPolicyType, rptPolicyString, "starttls-not-supported", hostAndIp.GetHostName());
            else if (tlsResult == SmtpTlsResult::HandshakeFailed || retryWithoutStartTls)
               TlsRptStore::Instance()->RecordFailure(recipientDomain, rptPolicyType, rptPolicyString, "validation-failure", hostAndIp.GetHostName());
         }

         if (retryWithoutStartTls && !serverInfo->GetRequireTls())
         {
            serverInfo->DisableConnectionSecurity();

            DeliverToSingleServer_(remainingRecipients, serverInfo, batchMessage);
         }

         // What the host said, read back: a reply that asks this server to
         // slow down opens the destination's back-off window, or - when this
         // delivery was the probe - doubles it. Only a reply a remote really
         // gave counts; a refused connection, a timeout and this server's own
         // reasons for not delivering leave the remote reply empty.
         if (backoffMinutes > 0)
         {
            String slowDownReply, slowDownHost;

            if (FindSlowDownReply_(remainingRecipients, slowDownReply, slowDownHost))
            {
               if (slowDownHost.IsEmpty())
                  slowDownHost = hostAndIp.GetHostName();

               DestinationBackoff::Entry entry;
               const bool changed = DestinationBackoff::Instance()->ReportSlowDown(target, probe.IsActive(), slowDownReply, slowDownHost,
                  backoffMinutes, backoffMaxMinutes, time(nullptr), entry);

               probe.SlowedDown();

               if (changed)
               {
                  LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": " + target + " asked this server to slow down (" + entry.reply + "). Deliveries to it are held for " + StringParser::IntToString(entry.window_minutes) + " minute(s), until " + DestinationBackoff::FormatTime(entry.until) + ".");
               }
            }
         }

         bool try_next_server = RecipientWithNonFatalDeliveryErrorExists_(vecRecipients);

         if (!try_next_server)
         {
            // All deliveries are complete or fatal. 
            return;
         }

         // Let's limit # of servers tried per retry to mxtries_factor_ * current number of retries to free up queue
         int iMXServerLimit = (original_message_->GetNoOfRetries()+1) * mxtries_factor_;
         if (mxtries_factor_ > 0 && i + 1 >= (unsigned int) iMXServerLimit )
         {
            LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Limiting to MXTriesFactored value of " + StringParser::IntToString(iMXServerLimit) + ".");      
            break;
         }
      }

      if (attemptedHosts == 0 && !mail_servers.empty())
      {
         // Every host was skipped - because its TLSA records failed DNSSEC
         // validation, or because a policy required DANE and none publishes a
         // usable record. Defer rather than deliver insecurely, and say which.
         LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Delivery to " + recipientDomain + " deferred: " + skippedReason + ". No host was attempted.");

         String errorMessage = _T("   Error Type: SMTP\r\n   Error Description: ") + skippedDescription + _T("\r\n\r\n");
         // As above: refused on security grounds, RFC 3463 X.7.0.
         HandleExternalDeliveryFailure_(vecRecipients, false, errorMessage, _T("4.7.0"));
      }
   }

   bool
   ExternalDelivery::EncryptBatch_(std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients,
                                   std::shared_ptr<RemoteDomainPolicy> encryption,
                                   std::shared_ptr<Message> &batchMessage,
                                   String &copyFile)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Gateway encryption for one batch: the partner's copy of the message,
   // encrypted to the key its remote domain policy holds. See
   // Common/Util/GatewayEncryption.h for what is written and why.
   //---------------------------------------------------------------------------()
   {
      // What the policy says, read once: one batch is sealed to one key with
      // one fallback. The record is the collection's; a REST or a COM save
      // replaces it with a new one and this batch keeps the one it was given
      // (COM/COMCollection.h, "A change to an item of a shared collection is
      // written onto a copy").
      const RemoteEncryption method = encryption->GetEncrypt();
      const String key = encryption->GetEncryptKey();
      const RemoteEncryptFallback fallback = encryption->GetEncryptFallback();
      const String policyName = encryption->GetDomainName();
      const String methodName = String(GatewayEncryption::MethodName(method));
      const String messageId = StringParser::IntToString(original_message_->GetID());

      const String candidate = FileUtilities::GetTempFileName();
      GatewayEncryption::KeyFacts facts;
      AnsiString problem;

      const GatewayEncryption::Outcome outcome = GatewayEncryption::SealFile(method, key,
         MessageStore::WorkingFile(original_message_), candidate, (__int64) time(nullptr), facts, problem);

      if (outcome == GatewayEncryption::AlreadyEncrypted)
      {
         LOG_APPLICATION("SMTPDeliverer - Message " + messageId + ": The remote domain policy for " + policyName + " asks for " + methodName +
            " encryption, and the message is encrypted end to end already. It is sent as it is.");
         return true;
      }

      if (outcome == GatewayEncryption::Sealed)
      {
         copyFile = candidate;

         // The queued message's own record with the copy's file: its sender, its
         // id for the log, its retry count. The recipients are the batch's, which
         // is who the copy is for - and what DKIMSigner asks when it decides
         // whether an ARC set is of use to anyone.
         std::shared_ptr<Message> copy = std::shared_ptr<Message>(new Message(*original_message_));
         copy->SetPartialFileName(candidate);

         // The copy is base64 and armour, whatever the original was.
         copy->SetBinaryMime(false);

         for (std::shared_ptr<MessageRecipient> recipient : vecRecipients)
            copy->GetRecipients()->Add(recipient);

         // DKIM signs a file of at most DKIM::MaxFileSize (50 MB) and passes a
         // larger one by without a word; the copy is about a third larger than
         // the original, so an original over about 36.5 MB makes a copy that
         // goes unsigned. Said in the log for that message, since the
         // original's own signature was taken off the copy's header.
         unsigned __int64 unsignedSize = 0;
         FileUtilities::FileSize64(candidate, unsignedSize);
         const bool overSigningCeiling = unsignedSize > (unsigned __int64) DKIM::MaxFileSize;

         // Signed afresh: the original's DKIM signature covered a body the
         // partner is not sent, and was taken off the copy's header with the ARC
         // set. The same signer PreprocessMessage_ runs, so the same domain signs
         // with the same key, or nothing signs because nothing would have.
         DKIMSigner signer;
         signer.Sign(copy);

         unsigned __int64 size = 0;
         FileUtilities::FileSize64(candidate, size);
         copy->SetSize((int) size);

         batchMessage = copy;

         // Both fingerprints for OpenPGP - the primary's, which the partner
         // publishes, and the subkey's, which is the one encrypted to.
         const String keyName = String(GatewayEncryption::KeyName(facts));

         LOG_APPLICATION("SMTPDeliverer - Message " + messageId + ": Encrypted with " + methodName + " to key " + keyName + " for " +
            StringParser::IntToString((int) vecRecipients.size()) + " recipient(s), as the remote domain policy for " + policyName + " requires.");

         if (overSigningCeiling)
         {
            LOG_APPLICATION("SMTPDeliverer - Message " + messageId + ": The encrypted copy for " + policyName + " is " +
               StringParser::IntToString((int) (unsignedSize / 1024)) + " KB, over the 50 MB that DKIM signs, and goes without a DKIM signature of this server's.");
         }

         for (std::shared_ptr<MessageRecipient> recipient : vecRecipients)
         {
            MessageTrace::Record(original_message_->GetID(), MessageTrace::EventEncrypted, original_message_->GetFromAddress(),
               recipient->GetAddress(), _sendersIP, 0, methodName + _T(" to key ") + keyName + _T(", remote domain policy ") + policyName);
         }

         return true;
      }

      FileUtilities::DeleteFile(candidate);

      // A sentence of its own, ended here by the text around it.
      String reason = String(problem);
      reason.TrimRight(_T('.'));

      if (outcome == GatewayEncryption::Deferred)
      {
         // A failure of this server's own - the message file locked by a
         // scanner, a full temporary directory, no memory, the cryptography
         // library - and not of the key: the next attempt may well succeed, so
         // the batch is retried, and neither refused nor sent in the clear,
         // whatever the fallback says. X.3.0 is RFC 3463's "other or undefined
         // mail system status", which is what this is.
         LOG_APPLICATION("SMTPDeliverer - Message " + messageId + ": Delivery deferred. The remote domain policy for " + policyName + " requires " + methodName +
            " encryption and the message could not be encrypted this time: " + reason + ". It will be retried; it has NOT been sent unencrypted.");

         String errorMessage;
         errorMessage.Format(_T("   Error Type: SMTP\r\n   Error Description: Delivery deferred. The remote domain policy for %s requires %s encryption to this destination, and the message could not be encrypted this time: %s. It will be retried, and has NOT been sent unencrypted.\r\n\r\n"),
            policyName.c_str(), methodName.c_str(), reason.c_str());

         HandleExternalDeliveryFailure_(vecRecipients, false, errorMessage, _T("4.3.0"));
         return false;
      }

      if (fallback == RemoteEncryptFallbackSendUnencrypted)
      {
         LOG_APPLICATION("SMTPDeliverer - Message " + messageId + ": The remote domain policy for " + policyName + " asks for " + methodName +
            " encryption and the message could not be encrypted: " + reason + ". It is sent UNENCRYPTED, as the policy's fallback allows.");

         for (std::shared_ptr<MessageRecipient> recipient : vecRecipients)
         {
            MessageTrace::Record(original_message_->GetID(), MessageTrace::EventSentUnencrypted, original_message_->GetFromAddress(),
               recipient->GetAddress(), _sendersIP, 0, _T("not encrypted (") + reason + _T("), remote domain policy ") + policyName);
         }

         return true;
      }

      LOG_APPLICATION("SMTPDeliverer - Message " + messageId + ": Not sent. The remote domain policy for " + policyName + " requires " + methodName +
         " encryption and the message could not be encrypted: " + reason + ".");

      // PERMANENT, as the size ceiling is: the key the message would be
      // encrypted to is the one the policy holds, and a retry finds the same
      // one; a message over the in-memory ceiling is no smaller next time.
      // The administrator's fix is a new key, and the sender learns now rather
      // than in two days. X.7.10 is the registry's "Encryption Needed".
      String errorMessage;
      errorMessage.Format(_T("   Error Type: SMTP\r\n   Error Description: The message was not sent. The remote domain policy for %s requires %s encryption to this destination, and the message could not be encrypted: %s. It has NOT been sent unencrypted.\r\n\r\n"),
         policyName.c_str(), methodName.c_str(), reason.c_str());

      HandleExternalDeliveryFailure_(vecRecipients, true, errorMessage, _T("5.7.10"));
      return false;
   }

   /// Resolves IP addresses for the recipient servers. This will either be a MX 
   /// lookup, or a A lookup, if SMTP relaying is used.
   bool 
   ExternalDelivery::ResolveRecipientServers_(std::shared_ptr<ServerInfo> &serverInfo, std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients, std::vector<HostNameAndIpAddress> &saMailServers)
   {
      DNSResolver resolver;

      // Resolve the specified hosts.
      bool dnsQueryOK = false;

      bool is_fixed = serverInfo->GetFixed();

      if (serverInfo->GetFixed())
      {
         String target_host_name = serverInfo->GetHostName();
         String target_ip_address = serverInfo->GetIpAddress();

         std::vector<String> mailServerHosts;

         bool useHostName = !target_host_name.IsEmpty();

         String relay_host_log_name;

         if (useHostName)
         {
            relay_host_log_name = target_host_name;

            if (target_host_name.Find(_T("|")) > 0)
               mailServerHosts = StringParser::SplitString(target_host_name, "|");
            else
               mailServerHosts.push_back(target_host_name);

            for(String host : mailServerHosts)
            {
               std::vector<String> ip_addresses;
               dnsQueryOK = resolver.GetIpAddresses(host, ip_addresses, true);

               for(String ip_address:  ip_addresses)
               {
                  HostNameAndIpAddress hostNameAndIpAddress;
                  hostNameAndIpAddress.SetHostName(host);
                  hostNameAndIpAddress.SetIpAddress(ip_address);

                  saMailServers.push_back(hostNameAndIpAddress);
               }
            }
         }
         else
         {
            String target_ip_address = serverInfo->GetIpAddress();
            relay_host_log_name = target_ip_address;

            HostNameAndIpAddress hostNameAndIpAddress;
            hostNameAndIpAddress.SetHostName("");
            hostNameAndIpAddress.SetIpAddress(target_ip_address);

            saMailServers.push_back(hostNameAndIpAddress);
         }

         LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Relaying to host " + relay_host_log_name + ".");      


      }
      else
      {
         // Resolve the mail server. The TCPConnection::Connect will normally do name
         // resolution, but since this is a matter of MX resolution and comparing
         // MX record preference, we have to do it manually.
         dnsQueryOK = resolver.GetEmailServers(serverInfo->GetHostName(), saMailServers);
      }

      std::shared_ptr<SMTPConfiguration> pSMTPConfig = Configuration::Instance()->GetSMTPConfiguration();
      const unsigned int maxNumberOfMXHosts = pSMTPConfig->GetMaxNumberOfMXHosts();

      if (maxNumberOfMXHosts > 0 && saMailServers.size() > maxNumberOfMXHosts)
      {
         LOG_DEBUG("Maximum number of MX host reached. Truncating MX server list.");
         saMailServers.resize(maxNumberOfMXHosts);
      }

      // Check if any servers exists.
      if (saMailServers.size() == 0)
      {
         HandleNoRecipientServers_(vecRecipients, dnsQueryOK, is_fixed);
         return false;
      }

      return true;
   }

   bool
   ExternalDelivery::RecipientWithNonFatalDeliveryErrorExists_(std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients)
   {
      // If there exists an recipient with nonfatal error,
      // we should try to deliver to other servers.
      auto iterRecipient = vecRecipients.begin();
      bool bTryNextServer = false;
      while (iterRecipient != vecRecipients.end())
      {
         std::shared_ptr<MessageRecipient> pRecipient (*iterRecipient);

         if (pRecipient->GetDeliveryResult() == MessageRecipient::ResultUndefined ||
            pRecipient->GetDeliveryResult() == MessageRecipient::ResultNonFatalError)
         {
            return true;
            break;
         }

         iterRecipient++;
      }

      return false;
   }

   void 
   ExternalDelivery::HandleExternalDeliveryFailure_(std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients,
                                                      bool bIsFatal,
                                                      String &sErrorString,
                                                      const String &enhancedStatusCode)
   {


      auto iterRecipient = vecRecipients.begin();
      while (iterRecipient != vecRecipients.end())
      {
         std::shared_ptr<MessageRecipient> pRecipient = (*iterRecipient);

         // Unless this recipient has already fatally failed, or succeeded,
         // update the state of it.

         bool bDeliveryComplete = pRecipient->GetDeliveryResult() == MessageRecipient::ResultOK ||
            pRecipient->GetDeliveryResult() == MessageRecipient::ResultFatalError;
         if (!bDeliveryComplete)
         {
            if (bIsFatal)
               pRecipient->SetDeliveryResult(MessageRecipient::ResultFatalError);
            else
               pRecipient->SetDeliveryResult(MessageRecipient::ResultNonFatalError);

            pRecipient->SetDeliveryError(sErrorString, enhancedStatusCode);
         }

         iterRecipient++;
      }
   }

   void
   ExternalDelivery::DeferBatchWithoutCounting_(std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients, int minutes, String &sErrorString)
   {
      // RFC 3463 X.4.5 "Mail system congestion": too much mail for this route
      // right now, try later - which is what every shaping deferral means.
      HandleExternalDeliveryFailure_(vecRecipients, false, sErrorString, _T("4.4.5"));

      for (std::shared_ptr<MessageRecipient> recipient : vecRecipients)
      {
         if (recipient->GetDeliveryResult() == MessageRecipient::ResultNonFatalError)
            shaping_deferred_.insert(recipient->GetAddress());
      }

      if (minutes < 1)
         minutes = 1;

      if (minutes > shaping_deferral_minutes_)
         shaping_deferral_minutes_ = minutes;
   }

   bool
   ExternalDelivery::EveryFailureIsShaping_(const std::map<String,DeliveryFailure> &mapFailedDueToNonFatalError) const
   {
      if (shaping_deferred_.empty() || mapFailedDueToNonFatalError.empty())
         return false;

      for (const auto &failed : mapFailedDueToNonFatalError)
      {
         if (shaping_deferred_.find(failed.first) == shaping_deferred_.end())
            return false;
      }

      return true;
   }

   int
   ExternalDelivery::SpreadMinutes_() const
   {
      return 1 + (int) (original_message_->GetID() % 5);
   }

   long
   ExternalDelivery::SecondsSinceCreation_() const
   {
      // "YYYY-MM-DD HH:MM:SS", the local system date the row was written with.
      // Parsed by hand rather than through the C library's scanners, which
      // differ between the two platforms this builds on.
      const String created = original_message_->GetCreateTime();

      if (created.GetLength() < 19)
         return -1;

      struct tm moment = {};
      moment.tm_year = _ttoi(created.Mid(0, 4).c_str()) - 1900;
      moment.tm_mon = _ttoi(created.Mid(5, 2).c_str()) - 1;
      moment.tm_mday = _ttoi(created.Mid(8, 2).c_str());
      moment.tm_hour = _ttoi(created.Mid(11, 2).c_str());
      moment.tm_min = _ttoi(created.Mid(14, 2).c_str());
      moment.tm_sec = _ttoi(created.Mid(17, 2).c_str());
      moment.tm_isdst = -1;

      // A year before 2000 is not a creation time this server wrote - the ETRN
      // hold parks a message at 1901 - and mktime's own failure is -1.
      if (moment.tm_year < 100)
         return -1;

      const time_t when = mktime(&moment);
      if (when == (time_t) -1)
         return -1;

      return (long) (time(nullptr) - when);
   }

   String
   ExternalDelivery::SenderTenant_() const
   {
      const String sender = original_message_->GetFromAddress();

      std::shared_ptr<DomainAliases> aliases = ObjectCache::Instance()->GetDomainAliases();
      String domain = StringParser::ExtractDomain(aliases ? aliases->ApplyAliasesOnAddress(sender) : sender);
      domain.ToLower();

      // Not hosted here - relayed for an IP range or a route, forwarded from
      // outside, or the null sender - is the one tenant every outsider shares.
      if (domain.IsEmpty() || !CacheContainer::Instance()->GetDomain(domain))
         return _T("(external)");

      return domain;
   }

   bool
   ExternalDelivery::FindSlowDownReply_(const std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients, String &reply, String &host)
   {
      for (std::shared_ptr<MessageRecipient> recipient : vecRecipients)
      {
         if (recipient->GetDeliveryResult() != MessageRecipient::ResultNonFatalError)
            continue;

         const String remoteReply = recipient->GetRemoteSmtpReply();
         if (remoteReply.IsEmpty())
            continue;

         // The three-digit code the reply opens with; the enhanced status is
         // the one the client recorded from the same reply (DeliveryFailure's
         // parsing), or the class's undefined code when the remote sent none.
         const int replyCode = _ttoi(remoteReply.Left(3).c_str());

         if (DestinationBackoff::IsSlowDownReply(replyCode, recipient->GetEnhancedStatusCode(), remoteReply))
         {
            reply = remoteReply;
            host = recipient->GetRemoteMta();
            return true;
         }
      }

      return false;
   }

   void
   ExternalDelivery::HandleNoRecipientServers_(std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients, bool bDNSQueryOK, bool isSpecificRelayServer)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Takes care of the situation when no valid recipient server addresses exist.
   //---------------------------------------------------------------------------()
   {
      LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": No mail servers could be found for the address " + (*vecRecipients.begin())->GetAddress() + ".");

      String bounceMessageText;

      // The enhanced status code alongside it. Every one of these is class 4,
      // and deliberately so even where the condition looks permanent: the loop
      // below forces a NON-FATAL delivery result whatever DNS said, so the
      // message is retried, and a 5.x.x code paired with a retry would tell the
      // sender's software that the address is dead while this server is still
      // trying it. The class the report carries has to be the class this server
      // is actually acting on.
      String enhancedStatusCode;

      // Generate a string which will be included in the bounce message.

      if (bDNSQueryOK)
      {
         if (isSpecificRelayServer)
         {
            bounceMessageText = _T("   Error Type: SMTP\r\n   Error Description: The host specified as SMTP relay server could not be found. Please contact your server administrator.\r\n\r\n");
            // RFC 3463 X.3.5 "System incorrectly configured" - the relay host is
            // this installation's own setting, and it does not resolve.
            enhancedStatusCode = _T("4.3.5");
         }
         else
         {
            bounceMessageText = _T("   Error Type: SMTP\r\n   Error Description: No mail servers appear to exists for the recipient's address.\r\n   Additional information: Please check that you have not misspelled the recipient's email address.\r\n\r\n");
            // RFC 3463 X.1.2 "Bad destination system address" - DNS answered, and
            // the answer was that the recipient's domain publishes nowhere to
            // deliver mail.
            enhancedStatusCode = _T("4.1.2");
         }
      }
      else
      {
         bounceMessageText = _T("   Error Type: SMTP\r\n   Error Description: Unable to find the recipient's email server. The DNS query has failed.\r\n\r\n");
         // RFC 3463 X.4.4 "Unable to route" - its text names precisely this: the
         // routing information was unavailable from the directory server.
         enhancedStatusCode = _T("4.4.4");
      }

      // Update the recipients with the bounce message text and delivery result.
      for(std::shared_ptr<MessageRecipient> recipient : vecRecipients)
      {
         // Temp change to force non fatal no matter DNS result
         // Messages bouncing immediately due to no mail servers due to DNS issue
         recipient->SetDeliveryResult(MessageRecipient::ResultNonFatalError);
         // recipient->SetDeliveryResult(bDNSQueryOK ? MessageRecipient::ResultFatalError : MessageRecipient::ResultNonFatalError);
         recipient->SetDeliveryError(bounceMessageText, enhancedStatusCode);
      }
   }

   SmtpTlsResult
   ExternalDelivery::DeliverToSingleServer_(std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients,
                                            std::shared_ptr<ServerInfo> serverInfo,
                                            std::shared_ptr<Message> message)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Connects to a remote server and delivers the message to it.
   //---------------------------------------------------------------------------()
   {
      LOG_DEBUG(Formatter::Format("Starting external delivery process. Server: {0} ({1}), Port: {2}, Security: {3}, User name: {4}", 
         serverInfo->GetHostName(), 
         serverInfo->GetIpAddress(), 
         serverInfo->GetPort(), 
         serverInfo->GetEffectiveConnectionSecurity(), 
         serverInfo->GetUsername()));

      std::shared_ptr<IOService> pIOService = Application::Instance()->GetIOService();

      std::shared_ptr<Event> disconnectEvent = std::shared_ptr<Event>(new Event()) ;
      std::shared_ptr<SMTPClientConnection> pClientConnection 
         = std::shared_ptr<SMTPClientConnection> (new SMTPClientConnection(serverInfo->GetEffectiveConnectionSecurity(), pIOService->GetIOContext(), pIOService->GetClientContext(), disconnectEvent, serverInfo->GetHostName()));

      // Apply TLS policy requirements (MTA-STS / DANE / a remote domain policy)
      // for this connection.
      if (serverInfo->GetRequirePeerVerification())
         pClientConnection->SetRequirePeerVerification();

      if (serverInfo->GetAcceptUnauthenticatedTls())
         pClientConnection->SetAcceptUnauthenticatedTls();

      // Which rule required the TLS, if one did. Decides whether a failure to
      // get it is a deferral naming the rule or the permanent refusal that
      // required STARTTLS has always produced. See SMTPClientConnection.
      if (!serverInfo->GetTlsPolicyReason().IsEmpty())
         pClientConnection->SetTlsPolicyReason(serverInfo->GetTlsPolicyReason());

      if (!serverInfo->GetDaneRecords().empty())
         pClientConnection->SetDaneRecords(serverInfo->GetDaneRecords());

      std::shared_ptr<std::atomic<int> > tlsResult = std::make_shared<std::atomic<int> >(static_cast<int>(SmtpTlsResult::NotReached));
      pClientConnection->SetTlsResultSink(tlsResult);

      pClientConnection->SetDelivery(message, vecRecipients);

      if (!serverInfo->GetUsername().IsEmpty())
      {
         pClientConnection->SetAuthInfo(serverInfo->GetUsername(), serverInfo->GetPassword());

         // Outbound XOAUTH2 (the Microsoft 365 Basic-auth cutover): when this
         // destination is one the administrator configured OAuth for, the AUTH
         // step presents a bearer token instead of the password. The token is
         // fetched (or served from cache) HERE, on the delivery thread, the same
         // place the MTA-STS policy fetch already lives. A fetch failure is
         // reported and the delivery proceeds with LOGIN - which the provider
         // will refuse VISIBLY, putting the real error in front of the
         // administrator instead of a silent stall.
         String oauthHosts = IniFileSettings::Instance()->GetOutboundOAuth2Hosts();
         String destinationHost = serverInfo->GetHostName();

         bool oauthApplies = false;
         std::vector<String> hosts = StringParser::SplitString(oauthHosts, ",");
         for (String host : hosts)
         {
            host.TrimLeft();
            host.TrimRight();
            if (!host.IsEmpty() && host.CompareNoCase(destinationHost) == 0)
            {
               oauthApplies = true;
               break;
            }
         }

         if (oauthApplies)
         {
            String fixedToken = IniFileSettings::Instance()->GetOutboundOAuth2FixedToken();

            if (!fixedToken.IsEmpty())
            {
               pClientConnection->SetOAuthBearer(fixedToken);
            }
            else if (!IniFileSettings::Instance()->GetOutboundOAuth2TokenUrl().IsEmpty())
            {
               String token, tokenError;
               if (OutboundOAuth2TokenClient::Instance()->GetToken(token, tokenError))
               {
                  pClientConnection->SetOAuthBearer(token);
               }
               else
               {
                  ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5904, "ExternalDelivery::InitiateExternalConnection",
                     "Could not obtain an OAuth2 token for " + destinationHost + ": " + tokenError +
                     " The delivery will be attempted with password authentication, which this provider is expected to refuse.");
               }
            }
         }
      }

      // Determine what local IP address to use: the one of the remote's family,
      // if the setting or the rule names one.
      IPAddress remoteAddress;
      remoteAddress.TryParse(AnsiString(serverInfo->GetIpAddress()), false);

      IPAddress localAddress = GetLocalAddress_(remoteAddress);

      if (pClientConnection->Connect(serverInfo->GetIpAddress(), serverInfo->GetPort(), localAddress))
      {
         // Make sure we keep no references to the TCP connection so that it
         // can be terminated whenever. We're no longer own the connection.
         pClientConnection.reset();

         disconnectEvent->Wait();
      }

      LOG_DEBUG("External delivery process completed");

      return static_cast<SmtpTlsResult>(tlsResult->load());
   }

   IPAddress
   ExternalDelivery::GetLocalAddress_(const IPAddress &remote, bool logSkippedBind)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Determines what local IP address to use when delivering to this host: a
   // rule's bind-to-address action first, else SMTPDeliveryBindToIP, and of
   // either only an address of the remote's family. See
   // SMTPConfiguration::OutboundBindAddress for why the other family is skipped.
   // The IP warm-up asks the same question before the first connection, with
   // logSkippedBind false, so that the connection's own line is the only one.
   //---------------------------------------------------------------------------()
   {
      std::shared_ptr<SMTPConfiguration> pSMTPConfig = Configuration::Instance()->GetSMTPConfiguration();

      String smtpSettingBindToIP = pSMTPConfig->GetSMTPDeliveryBindToIP();
      String ruleBindToAddress = _globalRuleResult.GetBindToAddress();

      const String configured = !ruleBindToAddress.IsEmpty() ? ruleBindToAddress : smtpSettingBindToIP;

      if (configured.IsEmpty())
         return IPAddress();

      IPAddress chosen = SMTPConfiguration::OutboundBindAddress(configured, remote);

      // Said where the administrator will look for it: a bind that was asked for
      // and not applied is otherwise a setting that silently did nothing.
      if (chosen.IsAny() && logSkippedBind)
      {
         LOG_TCPIP(Formatter::Format("Not binding to {0} for the connection to {1}: the two addresses are of different families, so the system chooses the source address.",
            configured, String(remote.ToString())));
      }

      return chosen;
   }

   void 
   ExternalDelivery::CollectDeliveryResult_(const String &serverHostName,
                                             std::vector<std::shared_ptr<MessageRecipient> > &vecRecipients,
                                             std::vector<DeliveryFailure> &saErrorMessages,
                                             std::map<String,DeliveryFailure> &mapFailedDueToNonFatalError,
                                             std::set<String> &suppressFailureDsnAddresses)
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // This function is called after delivery has ended. It goes through the recipients
   // and collects information on how the delivery went (good or bad). If delivery went
   // good, the recipient is deleted from the database.
   //---------------------------------------------------------------------------()
   {
      LOG_DEBUG("Summarizing delivery result");

      // Check how the delivery went.
      for(std::shared_ptr<MessageRecipient> recipient : vecRecipients)
      {
         // DSN (RFC 3461): remember recipients that opted out of failure
         // notifications so that any eventual bounce can be suppressed.
         int notify = recipient->GetDSNNotify();
         bool wantsFailureDsn = (notify == MessageRecipient::DSNNotifyDefault) ||
                                ((notify & MessageRecipient::DSNNotifyFailure) != 0);
         if (!wantsFailureDsn)
            suppressFailureDsnAddresses.insert(recipient->GetAddress());

         if (recipient->GetDeliveryResult() == MessageRecipient::ResultOK)
         {
            AWStats::LogDeliverySuccess(_sendersIP, serverHostName, original_message_, recipient->GetAddress());

            // Delete this recipient from the database.
            //
            // Unchecked, and a recipient row that survives is what decides whether the
            // message is rescheduled - so this address is delivered to again on the
            // next pass, and again after that. Duplicate mail arriving repeatedly, to
            // one recipient of a message that went out correctly to the others, is
            // close to undiagnosable from the outside.
            if (!PersistentMessageRecipient::DeleteObject(recipient))
            {
               ErrorManager::Instance()->ReportError(ErrorManager::High, 6105, "ExternalDelivery::Perform",
                  Formatter::Format("Message {0} was delivered to {1} but that recipient could not be removed from the database, so it will be delivered to them again.",
                     original_message_->GetID(), recipient->GetAddress()));
            }
         }
         else if (recipient->GetDeliveryResult() == MessageRecipient::ResultNonFatalError)
         {
            // Every path that sets a non-fatal result also sets a status now.
            // The fallback exists so that a path added later without one still
            // produces a well-formed report - RFC 3463 X.0.0, "other undefined
            // status", which is what "we do not know" looks like in this
            // vocabulary and is not a claim about the recipient.
            const String status = recipient->GetEnhancedStatusCode().IsEmpty() ? String(_T("4.0.0")) : recipient->GetEnhancedStatusCode();

            RememberNonFatalFailure_(mapFailedDueToNonFatalError, recipient, recipient->GetErrorMessage(), status);

            // The attempt, in the message trace, with the status and what the
            // remote said - or this server's own sentence where it said nothing -
            // so that "deferred, and why" is answerable while the message waits
            // (MessageTrace::EventDeferred; the send API reads it). Only an
            // attempt that was made: a recipient this server's own shaping held
            // back (a back-off window, a connection ceiling, a per-minute rate)
            // was tried by nobody, is rescheduled every few minutes without being
            // counted, and would write a row each time for as long as the hold
            // lasts.
            if (shaping_deferred_.find(recipient->GetAddress()) == shaping_deferred_.end())
            {
               const String said = recipient->GetRemoteSmtpReply().IsEmpty() ? recipient->GetErrorMessage() : recipient->GetRemoteSmtpReply();
               MessageTrace::Record(original_message_->GetID(), MessageTrace::EventDeferred, original_message_->GetFromAddress(),
                  recipient->GetAddress(), _sendersIP, MessageTrace::ReplyCode(recipient->GetRemoteSmtpReply(), 4, 451),
                  MessageTrace::DeliveryDetail(status, said));
            }
         }
         else if (recipient->GetDeliveryResult() == MessageRecipient::ResultFatalError)
         {
            // Yes, this is a permanent error.

            // DSN (RFC 3461): only generate a failure notification when the sender
            // has not opted out via NOTIFY=NEVER (or a NOTIFY list without FAILURE).
            int notify = recipient->GetDSNNotify();
            bool sendFailureDsn = (notify == MessageRecipient::DSNNotifyDefault) ||
                                  ((notify & MessageRecipient::DSNNotifyFailure) != 0);

            if (sendFailureDsn)
            {
               String sSingleErrorMsg;
               String sRecipient = recipient->GetAddress();
               sSingleErrorMsg = sRecipient + "\r\n";
               sSingleErrorMsg = sSingleErrorMsg + recipient->GetErrorMessage();
               sSingleErrorMsg = sSingleErrorMsg + "\r\n";

               // The status the remote server's reply produced. Only
               // SMTPClientConnection can reach a fatal result, and it always
               // records one, but the fallback is the honest undefined permanent
               // code rather than a guess at which of the 5.x.x conditions it was.
               DeliveryFailure failure(sRecipient,
                  recipient->GetEnhancedStatusCode().IsEmpty() ? String(_T("5.0.0")) : recipient->GetEnhancedStatusCode(),
                  sSingleErrorMsg);

               failure.SetRemoteSmtpReply(recipient->GetRemoteSmtpReply());
               failure.SetRemoteMta(recipient->GetRemoteMta());

               saErrorMessages.push_back(failure);
            }

            // Delete this recipient from the database. As above: a row that survives
            // is retried, so a permanent failure would be retried and bounced again.
            if (!PersistentMessageRecipient::DeleteObject(recipient))
            {
               ErrorManager::Instance()->ReportError(ErrorManager::High, 6105, "ExternalDelivery::Perform",
                  Formatter::Format("Delivery of message {0} to {1} failed permanently, but that recipient could not be removed from the database, so the attempt and its bounce will be repeated.",
                     original_message_->GetID(), recipient->GetAddress()));
            }

            // The trace carries the status and the remote's reply as well as the
            // code (MessageTrace::DeliveryDetail): "bounced, and why" is the
            // question the send API answers from it. The code is the remote's own
            // 5xx where its reply began with one, as the report groups failures
            // by it, and 550 - what was always recorded here - where it did not.
            {
               const String status = recipient->GetEnhancedStatusCode().IsEmpty() ? String(_T("5.0.0")) : recipient->GetEnhancedStatusCode();
               const String said = recipient->GetRemoteSmtpReply().IsEmpty() ? recipient->GetErrorMessage() : recipient->GetRemoteSmtpReply();

               AWStats::LogDeliveryFailure(_sendersIP, original_message_->GetFromAddress(), recipient->GetAddress(),
                  MessageTrace::ReplyCode(recipient->GetRemoteSmtpReply(), 5, 550), original_message_->GetID(),
                  MessageTrace::DeliveryDetail(status, said));
            }
            Events::FireOnDeliveryFailed(original_message_, _sendersIP, recipient->GetAddress(), recipient->GetErrorMessage());
         }
         else
         {
            // RFC 3463 X.4.2 "Bad connection". Reached when the recipient came
            // back undefined - nothing in the session ever gave a verdict for it.
            RememberNonFatalFailure_(mapFailedDueToNonFatalError, recipient, _T("Remote server closed connection."), _T("4.4.2"));

            MessageTrace::Record(original_message_->GetID(), MessageTrace::EventDeferred, original_message_->GetFromAddress(),
               recipient->GetAddress(), _sendersIP, 451, MessageTrace::DeliveryDetail(_T("4.4.2"), _T("Remote server closed connection.")));
         }

      }

      LOG_DEBUG("Summarized delivery results");
   }

   /// Checks if we should reschedule the message for later delivery. If so, we do.
   /// Returns true if the message is rescheduled.
   bool
   ExternalDelivery::RescheduleDelivery_(std::map<String,DeliveryFailure> &mapFailedDueToNonFatalError, std::vector<DeliveryFailure> &saErrorMessages, std::set<String> &suppressFailureDsnAddresses)
   {

      LOG_DEBUG("SD::RescheduleDelivery_");

      // We have failed recipients. Iterate over one of them at a time
      long iMaxNoOfRetries = 0;
      long lMinutesBewteen = 0;
      int iCurNoOfRetries = original_message_->GetNoOfRetries() ;

      quick_retries_ = IniFileSettings::Instance()->GetQuickRetries();
      quick_retries_Minutes = IniFileSettings::Instance()->GetQuickRetriesMinutes();
      queue_randomness_minutes_ = IniFileSettings::Instance()->GetQueueRandomnessMinutes();

      // Variables used to generate randomness value for retry delay
      errno_t rnd_err;
      unsigned int tmp_rnd = 0;
      int iRandomAdjust = 0;

      // See if randomness is enabled to work around Win2k compatability issue
      // plus saves work if not enabled which is default
      if (queue_randomness_minutes_ > 0)
      {

         // Get our random #
         // LOG_DEBUG("Windows 2000 does not support rand_s & pukes here");
         rnd_err = (rand_s(&tmp_rnd));

         // If no error getting random # use it
         if (rnd_err == 0)
            iRandomAdjust = (unsigned int) ((double)tmp_rnd / (double) UINT_MAX * queue_randomness_minutes_) + 1;
      }

      LOG_DEBUG("Retrieving retry options.");
      if (GetRetryOptions_(mapFailedDueToNonFatalError, iMaxNoOfRetries, lMinutesBewteen))
      {
         // so return now since no need for retry at this time

         // For now we unlock message here but might be best to do @ ETRN time..
         ReportWarmupHolds_(mapFailedDueToNonFatalError, _T("The message is held for ETRN."));
         PersistentMessage::UnlockObject(original_message_);

         LOG_APPLICATION("SMTPDeliverer - Route Message: HOLD for later delivery..");
         return true; // Do not delete e-mail now
      }

      // Every failure of this pass was a deferral this server chose - a
      // back-off window, a connection ceiling, a per-minute rate - and nothing
      // was offered to anybody. The next try is the deferral's own length, the
      // tries counter is untouched, and neither the quick-retry schedule nor
      // the randomness applies: the minutes are the window's. A pass in which
      // any recipient failed at a remote takes the counted path below, because
      // a remote failure is what the retry budget is for.
      //
      // Bounded, though: a message that has been queued for longer than the
      // span the counted schedule would have allowed it - (retries + 1) times
      // the minutes between tries - takes the counted path from then on, so
      // that a destination which answers "slow down" for good turns into the
      // bounce it always was, on the schedule's own clock, rather than a hold
      // for as long as the backlog lasts; this server sends no delayed
      // delivery report, so nobody would otherwise learn of it.
      if (EveryFailureIsShaping_(mapFailedDueToNonFatalError))
      {
         const long spanMinutes = (iMaxNoOfRetries + 1) * lMinutesBewteen;
         const long ageSeconds = SecondsSinceCreation_();

         if (spanMinutes > 0 && ageSeconds > spanMinutes * 60)
         {
            LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Deferred by this server's own shaping, but it has been queued for " + StringParser::IntToString((int) (ageSeconds / 60)) + " minute(s), longer than the " + StringParser::IntToString((int) spanMinutes) + " minute(s) its retry schedule spans. This deferral is counted, so that the message is not held for ever.");

            // Counted, but at the deferral's own length when the IP warm-up
            // held a recipient in this pass for longer than the retry
            // interval - to the next UTC day, above all. Counted at the retry
            // interval instead, the tries would come inside a day whose
            // ceiling cannot reopen, each one held again and spent, and the
            // message would be returned within hours of midnight with the next
            // day's room unused; this way each counted try lands on a new day,
            // and the retries are spent over as many days. A pass that only
            // the shaping row's deferrals held - a back-off window, a ceiling,
            // a rate, the tenant share - takes the counted path below exactly
            // as it did, whatever their length: this branch is theirs as well,
            // and the rule is the warm-up's alone.
            if (iCurNoOfRetries < iMaxNoOfRetries && warmup_hold_minutes_ > lMinutesBewteen)
            {
               LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Scheduling it for later delivery in " + StringParser::IntToString(shaping_deferral_minutes_) + " minute(s), the deferral's own length, since the IP warm-up's hold is longer than the retry interval; the attempt is counted against the retry limit.");

               ReportWarmupHolds_(mapFailedDueToNonFatalError, WarmupNextAttempt_(shaping_deferral_minutes_, true));

               PersistentMessage::SetNextTryTime(original_message_->GetID(), true, shaping_deferral_minutes_);
               PersistentMessage::UnlockObject(original_message_);

               return true; // Do not delete e-mail now
            }
         }
         else
         {
            const long shapingMinutes = shaping_deferral_minutes_ > 0 ? shaping_deferral_minutes_ : 1;

            LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Delivery deferred by this server's own shaping, not by a remote server. Scheduling it for later delivery in " + StringParser::IntToString((int) shapingMinutes) + " minute(s); the attempt is not counted against the retry limit.");

            ReportWarmupHolds_(mapFailedDueToNonFatalError, WarmupNextAttempt_(shapingMinutes, false));

            // The first-pass mark travels in the same statement, and onto the
            // cached object, so the next pass neither mirrors nor seals nor
            // notifies again - and the repeat mark with it, so that the rules
            // that ran on this pass do not forward, copy or reply again on the
            // next (Message::FlagRepeatsAttempt).
            PersistentMessage::SetNextTryTimeUncounted(original_message_->GetID(), shapingMinutes, true);
            original_message_->SetFirstPassDone(true);
            original_message_->SetRepeatsAttempt(true);

            PersistentMessage::UnlockObject(original_message_);

            return true; // Do not delete e-mail now
         }
      }

      if (iCurNoOfRetries < iMaxNoOfRetries)
      {
         // We should try at least once more - reschedule the message.
         LOG_DEBUG("Starting rescheduling.");

         // First few retries should be quicker for greylisting IF enabled
         if (iCurNoOfRetries < quick_retries_) 
         {
            LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Message could not be delivered. Greylisting? Scheduling it for quick retry " + StringParser::IntToString(iCurNoOfRetries + 1) + " of " + StringParser::IntToString(quick_retries_) + " in " + StringParser::IntToString(quick_retries_Minutes + iRandomAdjust) + " minutes.");
            ReportWarmupHolds_(mapFailedDueToNonFatalError, WarmupNextAttempt_(quick_retries_Minutes + iRandomAdjust, true));
            PersistentMessage::SetNextTryTime(original_message_->GetID(), true, quick_retries_Minutes + iRandomAdjust);
         
            // Unlock the message now so that a future delivery thread can pick it up.
            PersistentMessage::UnlockObject(original_message_);
         
            LOG_DEBUG("Message rescheduled for later quick delivery. (Greylisting?)");
            return true; // Do not delete e-mail now
         }
         else
         {
            // lMinutesBewteen is a long and iRandomAdjust an int, so the sum is a long.
            // MSVC binds IntToString(long) to the int overload, because long and int are
            // the same 32-bit type on Windows; clang finds int, unsigned int and __int64
            // all equally distant. The cast names the overload Windows already selects,
            // and a retry interval in minutes cannot overflow a 32-bit int.
            LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Message could not be delivered. Scheduling it for later delivery in " + StringParser::IntToString((int) (lMinutesBewteen + iRandomAdjust)) + " minutes.");
            ReportWarmupHolds_(mapFailedDueToNonFatalError, WarmupNextAttempt_(lMinutesBewteen + iRandomAdjust, true));
            PersistentMessage::SetNextTryTime(original_message_->GetID(), true, lMinutesBewteen + iRandomAdjust);
         
            // Unlock the message now so that a future delivery thread can pick it up.
            PersistentMessage::UnlockObject(original_message_);
         
            LOG_DEBUG("Message rescheduled for later delivery.");
            return true; // Do not delete e-mail now
         }
      }
      else
      {
         LOG_DEBUG("Aborting delivery.");

         // We are finished trying. Let's give up!
         LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": Message could not be delivered. Returning error log to sender.");

         // What the IP warm-up held says so in the report, with no next attempt.
         ReportWarmupHolds_(mapFailedDueToNonFatalError, _T("No further attempt is made: the message's retries are spent."));

         // Delivery failed the last time.
         //
         // One report per recipient, where this used to build a single block of
         // prose covering all of them. RFC 3464 needs a per-recipient record -
         // "which address failed, and with what status" is the whole question -
         // and there is no way back to that from a concatenated string.
         //
         // The wording is unchanged, including the trailing "Tried N time(s)".
         // For the single-recipient case, which is the overwhelming majority,
         // the text a person reads is byte-for-byte what it was. For several
         // recipients each now carries its own copy of that line instead of the
         // recipients sharing one at the end, which is what makes each block
         // self-contained enough to stand as its own recipient record.
         String sMsg;
         sMsg.Format(_T("Tried %d time(s)"), iMaxNoOfRetries+ 1);

         auto iterFailed = mapFailedDueToNonFatalError.begin();
         while (iterFailed != mapFailedDueToNonFatalError.end())
         {
            String sEmailAddress = (*iterFailed).first;
            const DeliveryFailure &pendingFailure = (*iterFailed).second;
            String sFailed = pendingFailure.GetText();

            // Delivery has failed for the last time. The trace's detail carries
            // the last attempt's status and what was said, as the report does,
            // and the code is the last reply's own - a 451 the remote kept
            // answering until the retries ran out - or 550, what was always
            // recorded here, where no remote replied.
            {
               const String said = pendingFailure.GetRemoteSmtpReply().IsEmpty() ? String(_T("Gave up after the last retry.")) : pendingFailure.GetRemoteSmtpReply();
               AWStats::LogDeliveryFailure(_sendersIP, original_message_->GetFromAddress(), sEmailAddress,
                  MessageTrace::ReplyCode(pendingFailure.GetRemoteSmtpReply(), 4, 550), original_message_->GetID(),
                  MessageTrace::DeliveryDetail(pendingFailure.GetEnhancedStatusCode(), said));
            }
            Events::FireOnDeliveryFailed(original_message_, _sendersIP, sEmailAddress, sFailed);

            // DSN (RFC 3461): suppress the bounce text for recipients that opted
            // out of failure notifications (NOTIFY=NEVER or NOTIFY without FAILURE).
            if (suppressFailureDsnAddresses.find(sEmailAddress) != suppressFailureDsnAddresses.end())
            {
               iterFailed++;
               continue;
            }

            String sErrorMessage = sEmailAddress + "\r\n" + sFailed;
            sErrorMessage += "\r\n";
            sErrorMessage += sMsg;
            sErrorMessage += "\r\n\r\n";

            // The status carried forward is the one the LAST attempt produced,
            // not a generic "gave up" code, because it is the more specific true
            // statement: 4.4.1 says nobody answered, 4.4.2 says the conversation
            // broke, 4.7.0 says a security policy blocked it. All of them are
            // class 4, which is the class RFC 3463 defines for exactly this - a
            // temporary condition that persisted until the attempts were
            // abandoned - and which keeps a list manager from unsubscribing an
            // address that was never shown to be bad.
            DeliveryFailure expired(sEmailAddress, pendingFailure.GetEnhancedStatusCode(), sErrorMessage);
            expired.SetRemoteSmtpReply(pendingFailure.GetRemoteSmtpReply());
            expired.SetRemoteMta(pendingFailure.GetRemoteMta());

            saErrorMessages.push_back(expired);

            iterFailed++;
         }

         // The recipients whose retries are spent are done with: their rows go
         // now, as a delivered or permanently refused recipient's do. The
         // message is usually deleted after this pass, and its rows with it -
         // but not when something else keeps it in the queue (local
         // recipients held while their mailbox's encryption is not known, a
         // posting the list manager could not read), and a row left then was
         // tried again on the next pass: another report, or a delivery after
         // the sender had been told it failed.
         {
            std::shared_ptr<MessageRecipients> recipients = original_message_->GetRecipients();
            if (recipients)
            {
               // owned by this call: the recipients of the message this delivery holds, read, not changed
               const std::vector<std::shared_ptr<MessageRecipient> > spent = recipients->GetVector();
               for (const std::shared_ptr<MessageRecipient> &recipient : spent)
               {
                  if (mapFailedDueToNonFatalError.find(recipient->GetAddress()) == mapFailedDueToNonFatalError.end())
                     continue;

                  if (!PersistentMessageRecipient::DeleteObject(recipient))
                  {
                     ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6105, "ExternalDelivery::RescheduleDelivery_",
                        Formatter::Format("Delivery of message {0} to {1} was given up when its retries ran out, and the sender is told; the recipient's row could not be removed from the database. That matters only when something else keeps the message in the queue - local recipients held, a posting the list manager could not read - and then this recipient is tried again on the next pass; otherwise the message, and the row with it, is deleted after this pass.",
                           original_message_->GetID(), recipient->GetAddress()));
                  }
               }
            }
         }

         LOG_DEBUG("Message not rescheduled for later delivery.")

        return false;
      }
   }

   String
   ExternalDelivery::WarmupHoldSentence_(const WarmupHold &hold)
   {
      String sentence;

      if (hold.held_by_others)
      {
         sentence.Format(_T("Delivery deferred by this server's IP warm-up: the address %s is on day %d of its %s schedule, which allows %d recipient(s) a day at %s, and the rest of today's allowance is held by deliveries from this address still in progress."),
            hold.address.c_str(), hold.day, hold.plan.c_str(), hold.ceiling, hold.provider.c_str());
      }
      else
      {
         sentence.Format(_T("Delivery deferred by this server's IP warm-up: the address %s is on day %d of its %s schedule, which allows %d recipient(s) a day at %s, and today's have been sent."),
            hold.address.c_str(), hold.day, hold.plan.c_str(), hold.ceiling, hold.provider.c_str());
      }

      return sentence;
   }

   String
   ExternalDelivery::WarmupNextAttempt_(long minutes, bool counted)
   {
      // Said from where the next try lands, which is known only once the
      // path is: after midnight UTC when it is on the next day, at the retry
      // interval when a counted try comes back inside this one.
      const __int64 now = (__int64) time(nullptr);
      const bool nextDay = IpWarmup::DayOf(now + (__int64) minutes * 60) > IpWarmup::DayOf(now);

      String sentence;

      if (nextDay)
         sentence.Format(_T("Next attempt after midnight UTC, in %d minute(s)"), (int) minutes);
      else if (counted)
         sentence.Format(_T("Next attempt in %d minute(s), at the retry interval"), (int) minutes);
      else
         sentence.Format(_T("Next attempt in %d minute(s)"), (int) minutes);

      sentence += counted ? _T("; the attempt is counted against the retry limit.") : _T("; the attempt is not counted against the retry limit.");

      return sentence;
   }

   void
   ExternalDelivery::ReportWarmupHolds_(std::map<String,DeliveryFailure> &mapFailedDueToNonFatalError, const String &nextAttempt)
   {
      for (const WarmupHold &hold : warmup_holds_)
      {
         String facts;
         facts.Format(_T("day %d of its %s schedule allows %d recipient(s) a day at %s, and "), hold.day, hold.plan.c_str(), hold.ceiling, hold.provider.c_str());
         facts += hold.held_by_others ? _T("the rest of today's allowance is held by deliveries from this address still in progress.") : _T("today's have been sent.");

         LOG_APPLICATION("SMTPDeliverer - Message " + StringParser::IntToString(original_message_->GetID()) + ": " +
            StringParser::IntToString((int) hold.addresses.size()) + " recipient(s) held by the IP warm-up of " + hold.address + ": " + facts + " " + nextAttempt);

         // The text a report would carry, which a pass's own path decides:
         // why, from the hold, and when, from here.
         const String text = _T("   Error Type: SMTP\r\n   Error Description: ") + WarmupHoldSentence_(hold) + _T(" ") + nextAttempt + _T("\r\n\r\n");

         for (const String &address : hold.addresses)
         {
            auto existing = mapFailedDueToNonFatalError.find(address);

            if (existing != mapFailedDueToNonFatalError.end())
               existing->second = DeliveryFailure(address, _T("4.4.5"), text);
         }
      }

      warmup_holds_.clear();
   }

   /// Returns the retry options for a list of address.
   /// The maximum number of retries and the maximum number of mintues between
   /// every try.
   // Type changed to bool for use in ETRN's
   bool 
   ExternalDelivery::GetRetryOptions_(std::map<String,DeliveryFailure> &mapFailedDueToNonFatalError, long &lNoOfRetries, long &lMinutesBetween)
   {
      std::shared_ptr<SMTPConfiguration> pSMTPConfig = Configuration::Instance()->GetSMTPConfiguration();
      std::shared_ptr<Routes> pRoutes = Configuration::Instance()->GetSMTPConfiguration()->GetRoutes();

      bool bFirstMatchingRoute = true;

      // First fetch the default values. Theese are used
      // if we can't find a route for any of the domains.
      lNoOfRetries = pSMTPConfig->GetNoOfRetries();
      lMinutesBetween  = pSMTPConfig->GetMinutesBetweenTry();

      auto iterAddress = mapFailedDueToNonFatalError.begin();
      std::map<String, std::shared_ptr<Route> > matchingRoutes;

      bool recipientsFoundNotMatchingRoute = false;

      while (iterAddress != mapFailedDueToNonFatalError.end())
      {
         String sAddress = (*iterAddress).first;
         String sDomainName = StringParser::ExtractDomain (sAddress).ToLower();
         
         std::shared_ptr<Route> pRoute = pRoutes->GetItemByNameWithWildcardMatch(sDomainName);

         if (pRoute)
         {
            int lTmpNoOfRetries = pRoute->NumberOfTries() - 1;
            int lTmpMinutesBetween = pRoute->MinutesBetweenTry();

            if (matchingRoutes.size() == 0)
            {
               lNoOfRetries = lTmpNoOfRetries;
               lMinutesBetween = lTmpMinutesBetween;
            }
            else
            {
               if (lTmpNoOfRetries > lNoOfRetries)
                  lNoOfRetries = lTmpNoOfRetries;

               if (lTmpMinutesBetween > lMinutesBetween)
                  lMinutesBetween = lTmpMinutesBetween;
            }

            matchingRoutes[sDomainName] = pRoute;
         }
         else
            recipientsFoundNotMatchingRoute = true;

         iterAddress++;
      }

      // If ONLY 1 route was found & not any non routes say we HOLD message otherwise don't.
      // HOLD when non-route recipient would be BAD. :D
      if (matchingRoutes.size() == 1 && !recipientsFoundNotMatchingRoute)
      {
         std::shared_ptr<Route> route = (*matchingRoutes.begin()).second;
         String routeDescription = route->GetDescription();

         if (routeDescription.ToUpper().StartsWith(_T("ETRN")))
         {
            __int64 iRouteID = route->GetID();

            // Here we change ID, type to 3 for HOLD. Retries reset to ensure it doesn't
            // bounce yet. NOT 0 though to stop mirror account copy over & over
            SQLCommand command("update hm_messages set messageaccountid = @ROUTEID, messagetype = 3, messagecurnooftries =  1,  messagenexttrytime = '1901-01-01 00:00:01' where messageid = @MESSAGEID");
            
            command.AddParameter("@ROUTEID", iRouteID);
            command.AddParameter("@MESSAGEID", original_message_->GetID());

            if (Application::Instance()->GetDBManager()->Execute(command))
            {
               // Execute OK - Should do some error checking & logging here..
            }

            return true;  // Say we HELD message
         }
         
         return false;
      }
      else
         return false;  // Continue as normal, no HOLD         
   }
}