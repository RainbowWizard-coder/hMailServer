// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The send API's record of what each API key sent, and what became of it
// (hm_apisends, schema 6089; RestApiSend.cpp; docs/SendApi.md).
//
// One row per message a key sent through POST /api/v1/send: the key's id, the
// queue id the message was saved under, its Message-ID, the envelope sender, the
// addresses the body named and when. The row is what makes "was message X
// delivered, deferred or bounced, and why" a question the KEY can ask - the
// message trace answers it for the server (GET /api/v1/message-trace/queue/{id})
// and is permissioned as the server's; this answers it for one key, about its
// own sends and nothing else.
//
// ONLY THE ADDRESSES THE BODY NAMED. A named address that resolved to others -
// a distribution list's members, an alias's target, the owner behind a masked
// address, a domain's catch-all - is answered for under its own name and never
// under theirs: those addresses are the list owner's and the mailbox owner's,
// and a key that could read them would unmask any masked address and list any
// list's members in any domain. What the queue and the trace hold about the
// addresses it resolved to is folded into the named address's entry - while the
// queue still holds them, through the original address each queue row keeps -
// with their state taken together and no reply of theirs shown. Once the queue
// has let them go, nothing ties a trace event back to the named address, and it
// reads unknown, saying so.
//
// WHAT A STATE IS MADE FROM. The message trace's events for the queue id -
// delivered, failed (bounced), suppressed (refused) and deferred, with the
// status and the remote's reply the trace holds in its detail
// (MessageTrace::DeliveryDetail), the newest MaxEvents of them - and, for what is
// still waiting, the queue itself: the message's row and the recipients it still
// has. So:
//
//   * With the trace off (MessageTraceEnabled, off by default) an address reads
//     queued or deferred while the queue holds it for that address and unknown
//     once the queue has let it go: nothing recorded what happened.
//   * A refusal on this server that writes no trace event - a full mailbox, a
//     rule, a virus scan - reads unknown, and the sentence says it may be that.
//   * A forward or a list's re-send is a new queue id, as the trace's own header
//     says, and is not followed.
//
// Kept MessageTraceRetentionDays and then removed (DeleteExpired, run hourly by
// RemoveExpiredRecords); 0 keeps them, as it keeps the trace. A key's rows go
// when the key is revoked through the key routes (DeleteForKey), and at the
// next hourly pass when it was revoked any other way (DeleteOrphans). An
// erasure of an address reaches them (EraseAddress).

#pragma once

#include <vector>

namespace HM
{
   class DALRecordset;

   struct ApiSendRecord
   {
      __int64 id = 0;
      String key_id;
      __int64 queue_id = 0;
      String message_id;
      String sender;
      std::vector<String> recipients;
      String created;
   };

   struct ApiSendRecipientState
   {
      String address;

      // queued, deferred, delivered, bounced, refused, or unknown.
      String state;

      // When the event that decided the state happened, as the trace holds it;
      // empty for an address that is only waiting.
      String when;

      // The RFC 3463 status and the SMTP code the trace holds for the event,
      // and what was said: the first 255 characters of the trace's detail - the
      // remote's reply, or the server's own sentence. Empty for an address that
      // resolved to others, whose replies are theirs.
      String status;
      int code = 0;
      String reason;
   };

   class ApiSends
   {
   public:

      // The most apsrecipients holds. Whole addresses only: an address that
      // would not fit is left out rather than cut, and is then not answered for.
      static const int RecipientsMaximum = 4000;

      // The most trace events one send's state is read from: the newest, which
      // hold each address's last word. A message deferred for days writes one
      // per attempt, and a reader must not load every one of them.
      static const int MaxEvents = 500;

      // One send, recorded once the message is in the queue. False when the row
      // could not be written; the caller says so.
      static bool Record(const String &keyId, __int64 queueId, const String &messageId,
                         const String &sender, const std::vector<String> &recipients);

      // The send of this key under that queue id. False - the same false - when
      // there is none, whether nobody sent it or another key did.
      static bool Find(const String &keyId, __int64 queueId, ApiSendRecord &record);

      // One page of this key's sends, newest first, and how many it has in all.
      // False when they could not be read.
      static bool List(const String &keyId, int offset, int count, std::vector<ApiSendRecord> &records, int &total);

      // Removes the rows older than MessageTraceRetentionDays. The number removed.
      static int DeleteExpired();

      // Removes every row of one key: what a revocation through the key routes
      // does. The number removed, or -1 when the database refused.
      static int DeleteForKey(const String &keyId);

      // Removes the rows of every key not among liveKeyIds - a key revoked by the
      // Control Panel or by an edit of the store. The number removed, or -1.
      static int DeleteOrphans(const std::vector<String> &liveKeyIds);

      // An erasure of an address (AddressTraceEraser): the rows it sent are
      // removed, and it is taken out of the recipients of the rest - a row left
      // naming nobody is removed. The number of rows changed or removed, or -1.
      static int EraseAddress(const String &address);

      // The state of one send and of each address it named, from the trace and
      // the queue - see the top of this file. traceEnabled says whether the
      // trace records events now.
      static String Describe(const ApiSendRecord &record, std::vector<ApiSendRecipientState> &recipients, bool &traceEnabled);

   private:

      static bool ReadRow_(std::shared_ptr<DALRecordset> recordset, ApiSendRecord &record);
      static int CountThenDelete_(const AnsiString &where, const AnsiString &parameter, const String &value);
   };
}
