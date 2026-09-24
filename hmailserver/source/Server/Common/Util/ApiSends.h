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
// WHAT A STATE IS MADE FROM. Nothing new is recorded at delivery for this: the
// states are read from the message trace's events for the queue id - delivered,
// failed (bounced), suppressed (refused) and deferred, with the status and the
// remote's reply the trace holds in its detail (MessageTrace::DeliveryDetail) -
// and, for what is still waiting, from the queue itself: the message's row and
// the recipients it still has. So two things are true of the answer that the
// document says as plainly:
//
//   * With the trace off (MessageTraceEnabled, off by default) a send reads
//     queued or deferred while it is in the queue and unknown once it has left
//     it: nothing recorded what happened.
//   * An address that resolved to others - an alias, a distribution list, a
//     forward - is reported under the addresses it resolved to, because those
//     are what the trace and the queue name; the address the body named then
//     reads unknown. A forward or a list's re-send is a new queue id, as the
//     trace's own header says, and is not followed.
//
// Kept MessageTraceRetentionDays and then removed (DeleteExpired, run hourly by
// RemoveExpiredRecords); 0 keeps them, as it keeps the trace.

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
      // and what was said: the remote's reply, or the server's own sentence.
      String status;
      int code = 0;
      String reason;
   };

   class ApiSends
   {
   public:

      // The most apsrecipients holds. Whole addresses only: an address that
      // would not fit is left out rather than cut, and the queue and the trace
      // still name it.
      static const int RecipientsMaximum = 4000;

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

      // The state of one send and of each address it concerns, from the trace
      // and the queue - see the top of this file. traceEnabled says whether the
      // trace records events now.
      static String Describe(const ApiSendRecord &record, std::vector<ApiSendRecipientState> &recipients, bool &traceEnabled);

   private:

      static bool ReadRow_(std::shared_ptr<DALRecordset> recordset, ApiSendRecord &record);
   };
}
