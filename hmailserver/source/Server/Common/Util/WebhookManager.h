// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Telling another program what this server's mail just did.
//
// Five things happen to mail that somebody else's software wants to know about
// the moment they happen - a message was accepted, a recipient was delivered
// to, a delivery failed for good, a message was held in the quarantine, an
// account was locked out - and until now the only way to learn any of them was
// to read a log file afterwards. A ticket system that opens a case when mail to
// a customer bounces, a CRM that files the receipt, a monitor that pages
// somebody when an account is locked: each of them is a program that wants a
// POST, not a log parser.
//
// WHY THIS IS NOT AlertManager
//
// AlertManager already signs, retries and dead-letters a webhook, and this
// reuses all three of those (SignWebhook and WebhookBackoffMinutes are called
// from here rather than copied). What does not fit is its SHAPE. Its webhook
// belongs to a RULE, one row per operational condition; it de-duplicates, so a
// condition that is already true is not raised again; and it has an hourly
// ceiling that diverts everything past it into a daily digest. Every one of
// those is right for "the disk is low" and wrong for a stream of messages:
// there is no such thing as the same delivery twice, and a digest of yesterday's
// deliveries is of no use to a program that was meant to act on them.
//
// So the shape here is a SUBSCRIPTION, and the differences are the point:
//
//   a subscription   a row in hm_webhooks: a name, an endpoint, a shared
//                    secret, which events it wants, which domains and which
//                    addresses it is confined to, whether it is on, and how
//                    many deliveries a minute it will accept. Several may want
//                    the same event, and each gets its own signed copy.
//
//   an event         raised by the path that OWNS it - the SMTP acceptance, the
//                    local and external deliverers, the quarantine store, the
//                    lockout - and never by anything that polls. Raising writes
//                    one row per matching subscription and returns; nothing is
//                    sent from the thread that noticed, for the reason
//                    AlertManager gives: a webhook about the delivery queue must
//                    not be POSTed by the thread the delivery queue is waiting on.
//
//   the queue        hm_webhookqueue: the body as it will be sent, the state,
//                    the attempts made, when the next one is due and the last
//                    failure. It is a table and not a list in memory because a
//                    restart between "the message arrived" and "the receiver was
//                    told" must not lose the second - and because that is what
//                    makes a receiver that was down for ten minutes get the ten
//                    minutes it missed.
//
//   the ceiling      per subscription, per minute. Past it an event is DROPPED,
//                    counted and said once, rather than queued. A queue that
//                    grows without bound is how a busy hour becomes a flood an
//                    hour later, and a receiver that cannot keep up is better
//                    told that it fell behind than told about it all afternoon.
//
// WHAT A BODY CARRIES, AND WHAT IT DOES NOT
//
// It does NOT carry the message. No body, no attachment, no header block, no
// recipient list beyond the one recipient the event is about. A webhook is a
// notification, not a copy, and an endpoint that wanted the mail should have
// been sent the mail.
//
// The one piece of content it can carry is the SUBJECT, and only where two
// separate switches agree: the domain's (Domain::GetWebhookSubjects, off on
// every domain until an administrator turns it on) and the subscription's
// (include_subject). Even then it is carried on message.received and
// message.quarantined alone - the two events whose owning path already has the
// subject in its hands - and never on a delivery or a failure, which name the
// same message by its queue id. Reading a subject the owning path does not
// already hold would put a file parse on every delivery, so the acceptance path
// asks WantsSubject() first and reads nothing when the answer is no.
//
// Everything else is envelope and outcome: who it was from, the one recipient,
// the domain, the size, the queue id, and - where the event has one - a status,
// a reason and a number.
//
// SIGNING
//
// Exactly AlertManager's recipe, because a receiver that already verifies an
// alert must not need a second implementation to verify a message event:
// HMAC-SHA256 over "<timestamp>.<body>" under the subscription's secret,
// lower-case hex, with the timestamp in X-hMailServer-Timestamp and the
// signature in X-hMailServer-Signature as "sha256=<hex>". X-hMailServer-Event
// names the event, X-hMailServer-Delivery the queue row - which is what lets a
// receiver recognise a retry of something it has already acted on - and
// X-hMailServer-Attempt which attempt this is.

#pragma once

#include <map>
#include <vector>

#include "../BO/ScheduledTask.h"

namespace HM
{
   class DALRecordset;

   class WebhookManager : public Singleton<WebhookManager>
   {
   public:

      WebhookManager();
      ~WebhookManager();

      enum QueueState
      {
         QueuePending = 1,
         QueueDelivered = 2,
         QueueDeadLettered = 3
      };

      // The five the roadmap names. They are constants rather than an enum on the
      // wire because they are what a receiver switches on, and a number would make
      // every subscription's configuration unreadable.
      static const wchar_t *EventMessageReceived;
      static const wchar_t *EventMessageDelivered;
      static const wchar_t *EventMessageBounced;
      static const wchar_t *EventMessageQuarantined;
      static const wchar_t *EventAccountLocked;

      static bool IsKnownEvent(const String &name);
      static std::vector<String> KnownEvents();

      struct Subscription
      {
         Subscription() : id(0), enabled(true), max_per_minute(0), include_subject(false), created(0) {}

         __int64 id;
         String name;            // the identifier in the REST path; unique
         String url;
         String secret;          // write-only over every surface; never read back
         String events;          // comma-separated; empty means every event
         String domains;         // comma-separated; empty means every domain
         String addresses;       // comma-separated; empty means every address
         bool enabled;
         int max_per_minute;     // 0: the server's WebhookMaxPerMinute
         bool include_subject;
         __int64 created;
      };

      struct Delivery
      {
         Delivery() : id(0), subscription_id(0), created(0), state(QueuePending), tries(0), next(0) {}

         __int64 id;
         __int64 subscription_id;
         String subscription;    // filled by GetDeliveries, not stored on the row
         String event;
         __int64 created;
         int state;
         int tries;
         __int64 next;
         String error;

         // The document, as characters. It is UTF-8 on the wire, and the one
         // conversion is made where it is sent: an AnsiString kept in memory or
         // in a column would be narrowed through the machine's code page by
         // CStdStr's conversion and back again, which loses every character the
         // page does not have - a subject in Greek, an address with an accent.
         String body;
      };

      // What happened, in the words of the path that owns it. Everything is
      // optional but the name: an event fills in what it knows and leaves the rest,
      // and BuildEventJson_ writes only the members that were filled in, so a
      // receiver never has to tell "absent" from "empty".
      struct Event
      {
         Event() : queue_id(0), size(0), number(0), has_number(false) {}

         String name;
         String domain;          // the local domain the event is about; scopes it
         String account;         // the local address, where the event has one
         String sender;
         String recipient;
         __int64 queue_id;
         __int64 size;
         String subject;         // carried only where the domain and the subscription agree
         String status;          // a delivery status or refusal code
         String reason;          // why: a quarantine reason, a lockout reason
         __int64 number;         // a spam score, the minutes a lockout lasts
         bool has_number;
      };

      // Records the event for every subscription that wants it, and returns how
      // many rows it wrote. Never sends anything, never opens a socket, and does
      // no database work at all when there is no subscription - which is every
      // installation that has not configured one. Safe to call from a protocol
      // thread, from a delivery thread and from the code that noticed.
      int Raise(const Event &event);

      // Whether anything would want the subject of an event about this domain. The
      // acceptance path asks before reading one out of the message file, so the
      // parse is paid for only by an installation that asked for subjects.
      bool WantsSubject(const String &domain, const String &eventName);

      // Which local domain and address an event about one message belongs to: the
      // recipient's when this server hosts it, otherwise the sender's when it hosts
      // that, otherwise neither. It is the recipient first because most events are
      // about mail arriving; the fallback is what makes a delivery to, or a bounce
      // from, the outside world still belong to the domain of ours that sent it.
      // Both are empty for a message that is neither from nor to a domain of this
      // server's, which only a relay sees - and a subscription confined to a domain
      // never matches one of those.
      static void ScopeToLocalDomain(const String &sender, const String &recipient, String &domain, String &account);

      // Whether any subscription at all would take this event. The owning paths ask
      // first so that a server with no subscription does not build an Event.
      bool Wants(const String &domain, const String &eventName);

      bool GetSubscriptions(std::vector<Subscription> &subscriptions);
      bool GetSubscription(const String &name, Subscription &subscription);

      // Writes the subscription under subscription.name. creating refuses a name
      // that exists; !creating refuses one that does not. Returns false with a
      // sentence when it is refused.
      bool SaveSubscription(const Subscription &subscription, bool creating, String &refusal);

      // Removes it, and with it (through the foreign key) everything of its that is
      // still queued. Returns false when there is no such subscription.
      bool DeleteSubscription(const String &name);

      // The queue, newest first. An empty name is every subscription; state 0 is
      // every state.
      bool GetDeliveries(const String &name, int state, int limit, std::vector<Delivery> &deliveries);

      // pending / delivered / dead-lettered for one subscription.
      bool CountDeliveries(__int64 subscriptionId, int &pending, int &delivered, int &deadLettered);

      // How many events this subscription has had dropped at its ceiling since the
      // server started. In memory, because it is a property of a rate rather than
      // of the configuration, and because it is a number nobody should be querying
      // history for - if it is not zero, the ceiling is wrong or the receiver is.
      int DroppedSinceStart(const String &name);

      // Everything WebhookTask does, in the order it does it. Separated so that a
      // test - and an administrator who has just configured an endpoint - need not
      // wait a minute for the scheduled pass.
      //
      // One pass at a time, in this process: the scheduled pass and a pass asked
      // for over REST run on different threads, and two passes reading the same
      // due rows would each POST them before either recorded the outcome - a
      // receiver would get two signed copies under one X-hMailServer-Delivery id,
      // and the second pass's outcome would overwrite the first's. The second
      // caller waits for the running pass to finish and then makes its own, which
      // finds whatever the first left.
      int RunDeliveries();
      int PurgeOldDeliveries();

      // Forgets the cached subscriptions, so that the next raise reads them again.
      // Called when one is written here, and by the REST and COM surfaces after a
      // change made anywhere else.
      void Invalidate();

      // The JSON one event is delivered as. Public because the documentation states
      // the shape and the tests pin it.
      static String BuildEventJson(const Subscription &subscription, const Event &event, __int64 when);

   private:

      bool ReadSubscriptions_(std::vector<Subscription> &subscriptions);
      bool CachedSubscriptions_(std::vector<Subscription> &subscriptions);
      static Subscription ReadSubscription_(const std::shared_ptr<DALRecordset> &recordset);
      static Delivery ReadDelivery_(const std::shared_ptr<DALRecordset> &recordset);

      static bool ListContains_(const String &list, const String &value);
      static bool Matches_(const Subscription &subscription, const Event &event);

      // True when this event may be queued for the subscription; false when the
      // subscription is at its ceiling for the current minute.
      bool WithinCeiling_(const Subscription &subscription);

      bool DomainAllows_(const String &domain, bool &subjectsAllowed);

      bool Enqueue_(const Subscription &subscription, const Event &event, __int64 now);
      bool UpdateDelivery_(__int64 id, int state, int tries, __int64 next, const String &error);

      // unreachable is set when no response was obtained at all - a refused
      // connection, a name that does not resolve, the deadline - as opposed to an
      // answer that was not a 2xx. The pass uses it to stop trying an endpoint
      // that is down rather than waiting one deadline per queued row.
      bool Deliver_(const Subscription &subscription, const Delivery &delivery, String &error, bool &unreachable);

      boost::recursive_mutex mutex_;

      // Held for the whole of RunDeliveries. Separate from mutex_, which the raise
      // path takes for a moment on every event and must never wait behind a POST.
      boost::mutex pass_mutex_;

      std::vector<Subscription> cached_;
      __int64 cached_at_;
      bool cache_valid_;

      struct Rate
      {
         Rate() : minute(0), count(0), dropped(0), said(0) {}

         __int64 minute;   // the minute (epoch / 60) the count belongs to
         int count;
         int dropped;      // since the server started
         __int64 said;     // the minute the ceiling was last written to the log
      };

      std::map<String, Rate> rates_;
   };

   class WebhookTask : public ScheduledTask
   {
   public:
      WebhookTask();
      ~WebhookTask();

      virtual void DoWork();
   };
}
