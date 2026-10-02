// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Mail in, to a webhook: an address, or a whole domain, whose mail is posted
// to a URL. SendGrid and Mailgun call it inbound parse; it is what an
// application actually buys a mail server for - a helpdesk that opens a ticket
// from support@, a CRM that files what orders@ receives, a service that reads
// replies to reply+<token>@ - and until now the only way to have it was a rule
// that ran a script. The outbound half is WebhookManager's: that tells another
// program what this server's mail DID; this hands it the mail.
//
// A ROUTE
//
//   a row of hm_inboundhooks: an address (orders@example.com) or a domain
//   (example.com) of a domain this server hosts; the URL the mail is posted
//   to; the shared secret it is signed under, protected at rest; whether the
//   mailbox the address has - where it has one - receives the message as well
//   (deliver), or the route alone does; what a message the endpoint never
//   accepted becomes - kept for the administrator, or returned to its sender
//   (on_failure); the shape of the body - multipart/form-data in SendGrid's
//   field names, or one JSON document (format); what is done with the
//   attachments - carried, named only, or left out (attachments); whether the
//   message as it arrived goes too (include_raw); a size ceiling of its own;
//   and whether the route is on.
//
//   An address route wins over a domain route, and a recipient written with a
//   plus tag (reply+1234@example.com) is found by the address before the tag
//   when there is no route for it as written. An address route decides whether
//   the address exists at all: RCPT TO is accepted for it whether or not an
//   account, an alias or a list has that address, and for a route that posts
//   only, the address's own mailbox - if it has one - receives nothing.
//
// HOW MAIL REACHES ONE
//
//   RecipientParser gives a routed address a recipient of its own, with no
//   account behind it, at RCPT TO and whenever a message is expanded - so an
//   alias or a list whose member is a routed address reaches the route too.
//   SMTPDeliverer hands the message here after the global rules, the virus
//   scan and the sandbox have had it, and before the list manager and local
//   delivery - the order RCPT TO asks in: every such recipient is taken off
//   the message, and each route's share of them becomes one row of
//   hm_inboundhookqueue with a copy of the message made through the message
//   store (MessageStore::CopyOriginal, to a working file in the queue
//   directory, which is never read as a container). A route that delivers as
//   well puts back what the address reaches without it - asked as RCPT TO
//   asks, a list's posting rules and its moderation included, with the routes
//   that have the message already left out - so the mailbox copy, or a
//   managed list's, is delivered by the list manager and local delivery in the
//   same pass as any other. Nothing is POSTed from the delivery thread: the
//   pass below does it, first from a drain posted at once to the asynchronous
//   queue and then once a minute, as the outbound webhooks' pass does.
//
// THE BODY, THE SIGNATURE, THE RETRIES
//
//   The body is built from the kept copy when an attempt is made - the parsed
//   parts through MessageData, the raw bytes through MessageStore::ReadAll - and
//   built the same way every time, its multipart boundary taken from the
//   SHA-256 of the message, so a retry is the same bytes under a fresh
//   timestamp. It is signed exactly as an outbound webhook is
//   (AlertManager::SignWebhook: HMAC-SHA256 over "<timestamp>.<body>" under the
//   route's secret), with X-hMailServer-Event: message.inbound. The retries are
//   the outbound webhooks' - WebhookMaxAttempts, and AlertManager's ladder of
//   0, 1, 5, 15, 60 minutes - and so is the retention of what was delivered,
//   WebhookRetentionDays. What the endpoint never accepted is KEPT (the copy
//   stays, readable and retried from REST and the Deck, and never swept) or
//   BOUNCED (returned to its sender by the delivery-failure report every other
//   failed delivery sends, and the copy removed), as the route says.
//
// WHERE AN ENDPOINT MAY BE
//
//   HttpsClient::RequestNotLinkLocal for a route the server's administrator
//   made - this server's own network is allowed, as an outbound webhook's is,
//   but never a link-local or cloud metadata address - and
//   HttpsClient::RequestPublicThroughProxy for one any other credential made,
//   an API key among them: such a route reaches public addresses only, so a
//   key cannot point this server at the inside of its own network, and goes
//   through HttpProxy when one is set. Neither follows a redirect. Plain http
//   only to this machine, as everywhere else.

#pragma once

#include <map>
#include <set>
#include <vector>

#include "../BO/ScheduledTask.h"
#include "../../SMTP/DeliveryFailure.h"

namespace HM
{
   class DALRecordset;
   class Message;
   class MessageRecipient;

   class InboundWebhooks : public Singleton<InboundWebhooks>
   {
   public:

      InboundWebhooks();
      ~InboundWebhooks();

      enum QueueState
      {
         QueuePending = 1,
         QueueDelivered = 2,
         // Never accepted, and the copy kept for the administrator: retried or
         // removed by hand, and never swept.
         QueueKept = 3,
         // Never accepted, and returned to its sender; the copy is gone.
         QueueBounced = 4
      };

      enum FailureAction
      {
         FailureKeep = 0,
         FailureBounce = 1
      };

      enum BodyFormat
      {
         FormatForm = 0,
         FormatJson = 1
      };

      enum AttachmentPolicy
      {
         AttachmentsInclude = 0,
         AttachmentsNames = 1,
         AttachmentsNone = 2
      };

      // The X-hMailServer-Event every request carries.
      static const wchar_t *EventName;

      // The largest message a route posts, in KB: the 80 MB MessageData parses,
      // and so the most a body can be built from. A route's own ceiling is at
      // most this, and 0 - no ceiling of the route's own - means this.
      static constexpr int MaxSizeKb = 81920;

      struct Route
      {
         Route() : id(0), deliver(false), on_failure(FailureKeep), format(FormatForm), attachments(AttachmentsInclude),
                   include_raw(false), max_size_kb(0), private_network(false), enabled(true), created(0) {}

         __int64 id;
         String address;         // lower case: user@domain, or a domain alone
         String url;
         String secret;          // write-only over every surface; never read back
         bool deliver;           // the address's mailbox receives the message as well
         int on_failure;         // FailureAction
         int format;             // BodyFormat
         int attachments;        // AttachmentPolicy
         bool include_raw;
         int max_size_kb;        // 0: no ceiling of the route's own, so MaxSizeKb
         bool private_network;   // made by the server's administrator: may reach this server's own network
         bool enabled;
         __int64 created;
      };

      struct Delivery
      {
         Delivery() : id(0), route_id(0), created(0), state(QueuePending), tries(0), next(0), queue_id(0), spam(false), size(0) {}

         __int64 id;
         __int64 route_id;
         String route;           // filled by the reads, not stored on the row
         __int64 created;
         int state;
         int tries;
         __int64 next;
         String error;
         String file;            // the kept copy's partial name; empty once it is gone
         __int64 queue_id;       // the delivery queue's id for the message
         String sender;
         String recipients;      // comma-separated: the route's share of the recipients
         String quiet;           // comma-separated: those of them whose DSN NOTIFY leaves out FAILURE
         String sender_ip;
         bool spam;
         __int64 size;
      };

      // ---- Mail -----------------------------------------------------------------

      // The enabled route that takes mail for this address: the address as
      // written, then the address without its plus tag, then its domain. False
      // when none does, which is every address on a server with no route - and
      // then this is one lock and one empty map. Safe from a protocol thread.
      bool FindRoute(const String &address, Route &route);

      // Takes every routed recipient off the message, keeps a copy of the
      // message for each route's share and queues it, and puts back what the
      // address reaches without its route where the route delivers as well -
      // again and again while that reaches another route, each route taking the
      // message once. Called before the list manager, so that a managed list
      // put back is the list manager's to distribute. A recipient whose copy
      // could not be kept, and every route's recipient while the routes cannot
      // be read at all, is put in held and left in the message, for SetAside
      // and HoldForRetry, and the list manager is told to leave it alone. A
      // route's recipient no route takes any more - switched off, or removed
      // since the message was accepted - is what the address is without one:
      // its mailbox, or returned as an unknown address. A message over a
      // bouncing route's ceiling is returned at once. Both reports go while the
      // queue file is still the message's: local delivery may take it for a
      // mailbox next. A message with no recipient a route could have made -
      // nearly every one - costs one look at its recipients.
      void TakeRoutedRecipients(std::shared_ptr<Message> message,
                                std::vector<std::shared_ptr<MessageRecipient> > &held);

      // After local delivery, as ListManager::SetAside: the held recipients go
      // from the message in memory so that nothing sends them outside, and their
      // rows stay.
      static void SetAside(std::shared_ptr<Message> message, const std::vector<std::shared_ptr<MessageRecipient> > &held);

      // The held recipients' retry, on the message's own schedule, as
      // ListManager::HoldForRetry; when the retries are spent the sender is told
      // (4.3.0) through failures. Returns whether the message is rescheduled.
      static bool HoldForRetry(std::shared_ptr<Message> message, const std::vector<std::shared_ptr<MessageRecipient> > &held,
                               bool alreadyRescheduled, std::vector<DeliveryFailure> &failures);

      // ---- The routes -----------------------------------------------------------

      bool GetRoutes(std::vector<Route> &routes);
      bool GetRoute(const String &address, Route &route);

      // Writes the route under route.address. creating refuses an address that
      // has a route; !creating refuses one that has none. mayReachPrivateNetwork
      // is whether the credential saving it is the server's administrator: when
      // it is not, the URL must lead to public addresses only, and the route is
      // stored so. Returns false with a sentence when it is refused.
      bool SaveRoute(const Route &route, bool creating, bool mayReachPrivateNetwork, String &refusal);

      // Removes the route, its queue (through the foreign key) and every copy
      // its queue kept. False when there is no such route.
      bool DeleteRoute(const String &address);

      // Every route of a domain being deleted - the domain's own and its
      // addresses' - as DeleteRoute removes one, so that a domain made again
      // under the name finds none. False when one could not be removed.
      bool DeleteRoutesOfDomain(const String &domainName);

      // The domain a route's address names: the part after the @, or the
      // address itself when it is a domain.
      static String DomainOf(const String &routeAddress);

      // Lower case, trimmed, and one of the two shapes: user@domain, or a
      // domain. Empty when it is neither.
      static String NormaliseAddress(const String &address);

      // ---- The queue ------------------------------------------------------------

      // Newest first. An empty address is every route; state 0 is every state.
      // onlyRoutes, when given, is the routes a caller confined to named
      // domains reaches: rows of any other route are passed over while reading,
      // so that limit counts the caller's own.
      bool GetDeliveries(const String &routeAddress, int state, int limit, std::vector<Delivery> &deliveries,
                         const std::set<__int64> *onlyRoutes = nullptr);
      bool GetDelivery(__int64 id, Delivery &delivery);

      // pending / delivered / kept / bounced for one route.
      bool CountDeliveries(__int64 routeId, int &pending, int &delivered, int &kept, int &bounced);

      // A kept delivery made due again with its attempts at none, or a pending
      // one made due now. Refused, with a sentence, for one that was delivered
      // or bounced, or whose copy is gone.
      bool RetryDelivery(__int64 id, String &refusal);

      // The row and the copy it kept.
      bool DeleteDelivery(__int64 id);

      // The kept copy, as the message arrived. False when there is none.
      bool ReadDeliveryMessage(__int64 id, AnsiString &bytes);

      // One pass: the due rows of the routes that are on, attempted within the
      // bounds WebhookManager's pass keeps. One pass at a time in this process,
      // and nobody waits for one: a pass asked for while another runs returns
      // -1 at once, and the running pass posts a drain when it ends if a
      // message was queued meanwhile. Otherwise how many were delivered.
      int RunDeliveries();

      // Delivered and bounced rows older than WebhookRetentionDays. Kept rows
      // are never swept: keeping them is what the route asked for.
      int PurgeOldDeliveries();

      // An erasure (AddressTraceEraser): every delivery the address sent, with
      // the message it kept, and the address taken out of the recipients of
      // the rest, a delivery left naming nobody going with its message. How
      // many rows changed, or -1 when the queue could not be read or changed.
      int EraseAddress(const String &address);

      void Invalidate();

      // Invalidate, and the drain forgotten, for Application::Reinitialize: the
      // asynchronous queue a drain was posted to is gone with the rest.
      void Forget();

      // The body of one delivery, as it is sent: the parsed parts, the
      // verdicts and, where the route asks, the raw message - built from the
      // copy alone, and so the same bytes on every attempt. raw is the copy's
      // bytes and file its name. Public because the documentation states the
      // shape and the tests pin it.
      static bool BuildBody(const Route &route, const Delivery &delivery, const AnsiString &raw, const String &file,
                            AnsiString &body, AnsiString &contentType, String &error);

      // Called by the drain task before its pass.
      void DrainStarting();

   private:

      bool ReadRoutes_(std::vector<Route> &routes);
      bool EnsureCache_();

      // FindRoute's lookup, with the routes that are switched off as well when
      // enabledOnly is false.
      bool FindRoute_(const String &address, Route &route, bool enabledOnly);
      static Route ReadRoute_(const std::shared_ptr<DALRecordset> &recordset);
      static Delivery ReadDelivery_(const std::shared_ptr<DALRecordset> &recordset);

      bool Spool_(std::shared_ptr<Message> message, const Route &route, const String &recipients, const String &quiet,
                  __int64 size, const String &senderIp, bool tooLarge, const String &tooLargeReason);
      // Puts back what the routed address reaches with no route, the routes in
      // routesTaken passed over, asked as RCPT TO asks. False when that is
      // nothing at all - or nothing the address's list lets this sender reach.
      bool ExpandForMailbox_(std::shared_ptr<Message> message, std::shared_ptr<MessageRecipient> routed,
                             const std::set<__int64> &routesTaken);
      static void TakeOff_(std::shared_ptr<Message> message, std::shared_ptr<MessageRecipient> recipient);

      // No account behind it, at a domain this server hosts: the shape of a
      // route's recipient - and of a managed list's, and of an SMTP route's.
      static bool IsCandidate_(const std::shared_ptr<MessageRecipient> &recipient);

      // Whether a recipient of that shape is the list manager's or an SMTP
      // route's rather than a route's that is gone.
      static bool IsSomebodyElses_(const String &address);

      // The ceiling a route holds a message to, in KB: its own, never above
      // MaxSizeKb, and MaxSizeKb when it has none.
      static int CeilingKb_(const Route &route);

      bool UpdateDelivery_(__int64 id, int state, int tries, __int64 next, const String &error, bool fileGone);
      bool Deliver_(const Route &route, const Delivery &delivery, String &error, bool &unreachable, bool &unreadable);

      // What a delivery the endpoint never accepted becomes. unreadable: the
      // copy is missing or no longer parses, so the endpoint never saw it.
      void GiveUp_(const Route &route, const Delivery &delivery, int tries, const String &error, bool unreadable);

      // Whether the row is still there: a delivery removed, or its route
      // removed, while a pass had it in hand is neither posted nor reported.
      bool StillQueued_(__int64 id);

      // RunDeliveries' pass, with pass_mutex_ held; and what is done when it is
      // let go of - a drain posted for what was queued meanwhile.
      int Pass_();
      void PassEnded_();

      // Removes one route's rows and copies. Called with its id read already.
      bool DeleteRouteById_(const Route &route);

      static String FileOf_(const String &partialName);

      void ScheduleDrain_();

      boost::recursive_mutex mutex_;

      // Held for the whole of a pass, and only ever tried: nothing waits for it.
      // Separate from mutex_, which the mail path takes for a moment on every
      // routed recipient and must never wait behind a POST.
      boost::mutex pass_mutex_;

      // The enabled and disabled routes alike, by lower-case address.
      std::map<String, Route> cached_;
      __int64 cached_at_;
      bool cache_valid_;

      // Under mutex_. draining_: a drain is posted and has not started. passing_:
      // a pass holds pass_mutex_. drain_wanted_: a message was queued since the
      // running pass read its rows, so it posts a drain when it ends. Together
      // they keep at most one drain on the asynchronous queue, and none of them
      // waiting for a pass.
      bool draining_;
      bool passing_;
      bool drain_wanted_;
   };

   class InboundWebhookTask : public ScheduledTask
   {
   public:
      InboundWebhookTask();
      ~InboundWebhookTask();

      virtual void DoWork();
   };

   class InboundWebhookDrainTask : public Task
   {
   public:
      InboundWebhookDrainTask();

      virtual void DoWork();
   };
}
