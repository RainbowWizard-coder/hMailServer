// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Sending a Web Push, and the only place in this server that does.
//
// THE SHAPE, WHICH IS THE ALERT WEBHOOK'S
//
//   noticing    LocalDelivery calls Note() when a message has been written into
//              a mailbox. That is one lock and one push_back on a bounded list:
//              nothing is read, nothing is signed and no socket is opened on the
//              delivery thread, for the same reason AlertManager::Raise writes a
//              row and returns. A mail server that waited on a browser vendor's
//              push service before finishing a delivery would be a mail server
//              that stops delivering when that service is slow.
//
//   sending     PushTask, once a minute, and a drain posted to the asynchronous
//              work queue as soon as something is noted - so the usual case is
//              seconds rather than a minute, and the minute is the floor that
//              catches retries and anything the drain missed.
//
//   failing     Exactly the alert webhook's backoff: no wait before the first
//              retry, then 1, 5, 15 and 60 minutes, and after WebPushMaxAttempts
//              the SUBSCRIPTION is dead-lettered - its row kept, with the reason,
//              so the person can see in the webmail why their phone went quiet -
//              and HM7002 is written once. A 404 or a 410 is not a failure to
//              retry: it is the push service saying the subscription is gone, and
//              the row is deleted.
//
// WHAT LEAVES THE SERVER
//
// Only what the person asked for, decided per device by
// PushSubscriptions::Wants and by the device's detail level. At the lowest level
// the encrypted payload holds a count and nothing else; at the highest it holds
// the sender and the subject. The push service sees neither: it sees an opaque
// body of a size it could have guessed anyway. docs/WebPush.md states the
// payload field by field, which is the promise this class keeps.

#pragma once

#include <deque>
#include <vector>

#include "../BO/ScheduledTask.h"
#include "PushSubscriptions.h"

namespace HM
{
   class Message;

   class PushNotifier : public Singleton<PushNotifier>
   {
   public:

      PushNotifier();
      ~PushNotifier();

      // A message has been delivered into a mailbox. Cheap by construction: it
      // takes a mutex, appends to a bounded list and returns. Does nothing at all
      // when Web Push is off.
      void Note(__int64 accountId, std::shared_ptr<const Message> message);

      // One pass: everything noted since the last one, to every device that wants
      // it and is not backing off. Returns how many pushes were accepted by a push
      // service. Separated from the task so a test can run a pass without waiting
      // for a minute to elapse.
      int SendPending();

      // How many notes are waiting. For the test that proves the feature being off
      // means nothing is queued, let alone sent.
      size_t PendingCount();

      // Everything waiting is dropped. Used when the feature is switched off and
      // by a test that wants a clean start.
      void Forget();

      // Called by PushDrainTask as its pass begins: the next thing noted posts a
      // drain of its own rather than waiting for the minute.
      void DrainStarting();

      // One push to one subscription, now, whatever is queued: what
      // POST /api/v1/me/push/test sends so that a person can find out whether
      // their phone is reachable before they rely on it. Returns false with a
      // sentence. Honours neither the quiet hours nor the sender list, and says so
      // in the documentation: a test the person asked for is not an interruption.
      bool SendTest(const PushSubscriptions::Subscription &subscription, String &error);

      // Minutes to wait before attempt n+1, the alert webhook's table: 0, 1, 5,
      // 15, 60, 60 ... Public because the documentation states it and a test pins
      // it.
      static int BackoffMinutes(int attemptsSoFar);
      static void RecordFailure_(__int64 subscriptionId, int failuresSoFar, __int64 now, const String &error);

      // The payload one message becomes at a given detail level, as JSON. Public
      // and pure: it is the promise about what leaves the server, and a test reads
      // it without a push service.
      static AnsiString BuildPayload(int detail, int count, const AnsiString &sender, const AnsiString &name,
                                     const AnsiString &subject, const AnsiString &folder, __int64 when);

      // The VAPID subject: what is configured, or a mailto: built from the
      // server's own host name when nothing is. A push service that has to tell
      // somebody their subscriptions are failing needs an address that reaches a
      // person.
      static AnsiString VapidSubject();

   private:

      struct Pending
      {
         Pending() : account_id(0), folder_id(0), message_id(0), noted(0) {}

         __int64 account_id;
         __int64 folder_id;
         __int64 message_id;
         __int64 noted;
         String sender;
         String keywords;
         String file_name;
      };

      // What one folder is: its name, and whether it is a folder new mail is
      // announced from at all.
      bool FolderIsAnnounced_(__int64 folderId, String &name);

      bool SendOne_(const PushSubscriptions::Subscription &subscription, const AnsiString &payload,
                    int &statusCode, String &error);

      // The subject line of a message file, decoded, at most 120 characters.
      static String SubjectOf_(const String &fileName);

      boost::recursive_mutex mutex_;

      std::deque<Pending> pending_;

      // Set while a drain is queued or running, so that a thousand deliveries in a
      // minute post one task rather than a thousand.
      bool draining_;
   };

   class PushTask : public ScheduledTask
   {
   public:
      PushTask();
      ~PushTask();

      virtual void DoWork();
   };

   // The drain posted to the asynchronous work queue when something is noted.
   class PushDrainTask : public Task
   {
   public:
      PushDrainTask();

      virtual void DoWork();
   };

   // Run from ClassTester::DoTests: the backoff table and the payload, which are
   // the two things stated in the documentation.
   class PushNotifierTester
   {
   public:
      void Test();
   };
}
