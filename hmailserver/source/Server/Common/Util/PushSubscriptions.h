// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The rows behind Web Push: one per browser or phone a person asked to be told
// on, and what each of them is allowed to be told.
//
// A subscription is made in the browser, not here. `pushManager.subscribe` hands
// the page three things - an endpoint at the browser vendor's push service, a
// P-256 public key (p256dh) and sixteen random octets (auth) - and the page
// posts them to /api/v1/me/push/subscriptions. They are secrets in the sense
// that anybody holding all three can send that device a notification this server
// signed for, which is why they are never emitted by any read: a listing gives
// the device's name, when it was made and what it is set to tell, and nothing
// that could be used to send to it.
//
// THE PERSON'S CHOICE LIVES ON THE ROW, PER DEVICE
//
// Not in the account's preferences, and that is deliberate. "Everything on my
// phone, only what is from work on the tablet, and nothing after ten at night"
// is the shape people actually want, and it cannot be said once per account. So
// each row carries: what a push may carry (a count, the sender, or the sender
// and the subject), which senders count, which labels count, and the quiet hours
// with the offset of the device's own clock from UTC - because quiet hours in
// the server's time zone are not quiet hours.
//
// The endpoint is looked up by a SHA-256 of itself rather than by its text. SQL
// Server Compact will not compare a long nvarchar parameter with a column, and a
// push endpoint is comfortably long enough to meet that; the hash is 64
// characters and is what the unique index is on.

#pragma once

#include <vector>

namespace HM
{
   class DALRecordset;

   class PushSubscriptions
   {
   public:

      enum State
      {
         StateActive = 1,
         StateDeadLettered = 3
      };

      // What a push to this device may say. Each level includes the one before
      // it, and the documentation says exactly what each one puts on the wire.
      enum Detail
      {
         DetailCount = 0,        // "1 new message" - no sender, no subject
         DetailSender = 1,       // and who it is from
         DetailSubject = 2       // and what it is about
      };

      struct Subscription
      {
         Subscription()
            : id(0), account_id(0), created(0), last_sent(0), state(StateActive), failures(0),
              next_attempt(0), detail(DetailSender), quiet_from(-1), quiet_to(-1), quiet_offset(0)
         {
         }

         __int64 id;
         __int64 account_id;
         String endpoint_hash;
         String endpoint;
         String p256dh;
         String auth;
         String device;
         __int64 created;
         __int64 last_sent;
         int state;
         int failures;
         __int64 next_attempt;
         String last_detail;
         int detail;
         String senders;      // empty: anyone. Otherwise a comma-separated list of addresses and @domains.
         String labels;       // empty: any. Otherwise a comma-separated list of IMAP keywords.
         int quiet_from;      // minutes past midnight, -1 when there are no quiet hours
         int quiet_to;
         int quiet_offset;    // minutes the device's clock is ahead of UTC
      };

      // Every subscription of one account, oldest first.
      static bool ForAccount(__int64 accountId, std::vector<Subscription> &subscriptions);

      // Every account that has at least one subscription which is not dead-lettered.
      // Read once per pass rather than per message.
      static bool ActiveAccounts(std::vector<__int64> &accountIds);

      static bool ById(__int64 accountId, __int64 id, Subscription &subscription);

      // Stores one, by its endpoint: a device that subscribes again - which a
      // browser does whenever the push service rotates its keys - replaces its own
      // row rather than making a second, which is why the unique index is on the
      // account and the endpoint hash rather than on the id alone. Returns false
      // with a sentence when the subscription is refused.
      //
      // fromThisMachine: the request came from this machine's own loopback. Only
      // then is a plain-http endpoint on a loopback address taken (IsLoopbackHttp):
      // someone on this machine reaches its loopback already, and anyone else
      // would be using the server to reach it for them.
      static bool Save(Subscription &subscription, String &refusal, bool fromThisMachine);

      // Plain http to 127.0.0.0/8, localhost or ::1, judged by the host of the
      // URL: the one exception to "an endpoint is https", and the same exception
      // HttpsClient makes. PushNotifier sends to such an endpoint with
      // HttpsClient::Request and to every other with RequestPublicThroughProxy.
      static bool IsLoopbackHttp(const String &endpoint);

      // The person's choice about an existing row; the keys are never changed by
      // this, only by a fresh subscription.
      static bool SaveChoice(__int64 accountId, __int64 id, int detail, const String &senders, const String &labels,
                             int quietFrom, int quietTo, int quietOffset, String &refusal);

      static bool Remove(__int64 accountId, __int64 id);

      // Removes by endpoint, which is what a 404 or 410 from the push service
      // means: the subscription is gone and will never work again.
      static bool RemoveByEndpoint(__int64 accountId, const String &endpoint);

      static int CountForAccount(__int64 accountId);

      // After an attempt: delivered (failures back to none), or failed (the
      // failure count and the time of the next attempt).
      static bool MarkDelivered(__int64 id, __int64 when);
      static bool MarkFailed(__int64 id, int failures, __int64 nextAttempt, int state, const String &detail);

      // The SHA-256 of an endpoint, lower-case hex: the lookup key.
      static String HashOf(const String &endpoint);

      // Whether this subscription would be told about a message from this sender
      // carrying these keywords, at this moment. Pure, and public, because it is
      // the whole of the person's choice and a test pins it without a push
      // service, a message or a clock.
      //
      // now is a Unix time; the quiet hours are judged in the device's own time,
      // which is now plus its stored offset.
      static bool Wants(const Subscription &subscription, const String &sender, const String &keywords, __int64 now);

      // Whether a moment falls inside the quiet hours. Separated because a window
      // that wraps midnight (22:00 to 07:00) is the common case and the one that
      // is easy to get wrong.
      static bool IsQuiet(const Subscription &subscription, __int64 now);

      // A comma-separated list of addresses and @domain suffixes, matched against
      // one sender address, case-insensitively. An empty list matches everything.
      static bool SenderMatches(const String &list, const String &sender);

      // A comma-separated list of IMAP keywords against the message's keywords.
      // An empty list matches everything.
      static bool LabelMatches(const String &list, const String &keywords);

   private:

      static Subscription Read_(const std::shared_ptr<DALRecordset> &recordset);
   };

   // Run from ClassTester::DoTests: the choice rules above, which are the part a
   // person will notice being wrong and the part no database is needed to check.
   class PushSubscriptionsTester
   {
   public:
      void Test();
   };
}
