// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Message recall within the server (Roadmap2 row 112, docs/MessageRecall.md):
// an unread message sent to accounts on this server can be withdrawn by its
// sender, which Outlook does within Exchange and nothing does across the
// internet. This server does it within itself, and nowhere else.
//
// WHAT IS RECORDED, AND WHEN. A message this server accepted from one of its
// own accounts - an authenticated SMTP submission, credited to the account
// that signed in (SMTPConnection, beside the compromise detector's note), or
// the webmail's send (HandleMeMessageSend_, which knows the account outright)
// - gets one row in hm_recall (schema 6085) for every recipient that
// resolved to a LOCAL account at acceptance: the sender's account id, the
// message's Message-ID without its angle brackets, the recipient's account
// id, the queue row the message was accepted as, and when. The recipient's
// hm_messages row id is 0 until the copy is delivered; LocalDelivery
// completes it at the point where the copy has been saved, and for a
// recipient reached by expansion whose row does not exist yet - a list
// member, an alias target - inserts one, provided rows for THE SAME QUEUE
// MESSAGE already exist and the delivered file carries the rows' Message-ID:
// that is how a member's copy is tied to its sender without trusting the
// From header, and without trusting the Message-ID alone, since a
// Message-ID is not a secret and anybody may write one.
//
// A COPY THE READER MAKES within their own mailbox - IMAP COPY and MOVE, the
// webmail's move, archive, junk and trash, which are all copy-and-delete -
// is the same message under a new hm_messages id. PersistentMessage marks
// the copy with the id it came from, and the save that assigns the new id
// writes a row beside the old one (Copied), so a recipient may hold several
// rows and the recall follows the message wherever they filed it. A copy
// into another account's folder is that account's and gets no row.
//
// Nothing is recorded for mail from outside, for mail this server writes,
// or for an unauthenticated submission from a local range in a local
// account's name: the sender's identity is the session's, not the
// envelope's. A message with a Message-ID longer than 255 characters is not
// recallable, and the log says so; one without a Message-ID would not be
// either, but a submission without one is given one at acceptance
// (SMTPMessageHeaderCreator) and the webmail always writes one.
//
// THE RECALL is POST /api/v1/me/recall (RestApiMessageRecall.cpp): the
// rows for (this account, that Message-ID) not older than RecallDays,
// judged PER RECIPIENT - read when any live copy of theirs carries \Seen
// (nothing is done, the reader keeps every copy), else recalled when at
// least one unread live copy was deleted the way the webmail deletes a
// message (every unread copy goes), else gone. Rows stay, marked with their
// outcome, and a second call answers the same. Nothing is sent to anybody:
// the Sent copy's own $Recalled keyword is the record. A recipient elsewhere
// is not in the table at all; the answer counts the addresses the Sent copy
// names that are not on this server.
//
// THE BOUNDS. RecallDays (14; 1 to 365): rows older than that are swept by
// the minute task and by the retention task, and a recall of an older
// message answers 404 as if it had never been recorded. At most 200 rows per
// sender per rolling day are recorded - a bulk sender's rows are not what
// this is for; past that nothing more is recorded for the day, and the
// application log says so once per sender per day.
//
// Read is \Seen and nothing more: a client that fetched the message without
// marking it seen - a POP3 client, an IMAP client that peeks - has it, and
// the recall cannot know. docs/MessageRecall.md says so in the row's words.
#pragma once

#include <vector>

namespace HM
{
   class Account;
   class Message;

   class MessageRecall
   {
   public:
      // A Message-ID longer than this is not recorded (the column is
      // nvarchar(255)), and a message carrying one is not recallable.
      static const int MessageIdMaximum = 255;

      // Rows recorded per sender per rolling day, at most.
      static const int RowsPerSenderPerDay = 200;

      static const int DaySeconds = 86400;

      enum Outcome
      {
         Pending = 0,     // recorded, not recalled
         Recalled = 1,    // the unread copy was deleted
         Read = 2,        // the recipient had read it; the copy was left
         Gone = 3         // the copy was not there to recall
      };

      struct Row
      {
         Row() : id(0), sender_id(0), recipient_id(0), queue_id(0), stored_id(0), created(0), recalled_at(0), outcome(Pending) {}

         __int64 id;
         __int64 sender_id;
         String message_id;      // without the angle brackets
         __int64 recipient_id;
         __int64 queue_id;       // the hm_messages row the message was accepted as
         __int64 stored_id;      // the recipient's hm_messages row; 0 until delivered
         __int64 created;        // seconds since 1970, UTC
         __int64 recalled_at;    // 0 until recalled
         int outcome;
      };

      // The RecallDays setting, 1 to 365.
      static int Days();

      // A Message-ID as the table holds it: trimmed, the angle brackets off.
      // Empty when there is nothing left or it is longer than the maximum.
      static String Canonical(const String &value);

      // The Message-ID of the message in the file, canonical; empty when the
      // header is missing or too long.
      static String MessageIdOf(const String &fileName);

      // At acceptance: one row per distinct local recipient of the message,
      // which has been saved (its id is the queue id). source names the door
      // for the log ("SMTP", "the webmail"). Answers how many rows were
      // written: 0 for a message with no local recipient, no Message-ID, or
      // a sender at the day's ceiling.
      static int Record(std::shared_ptr<const Account> sender, std::shared_ptr<Message> message, const AnsiString &source);

      // At local delivery, once the recipient's copy is saved: the row for
      // (this queue message, this recipient) gets the stored id; a recipient
      // with no row, reached by expansion, gets one when rows for the queue
      // message exist. The delivered file's Message-ID has to be the rows'
      // - a queue id a database reissued after a restart must not tie an
      // outsider's message to a sender's rows. Costs one indexed select per
      // local delivery and nothing more for a message nobody recorded; the
      // header is read only when rows exist.
      static void Delivered(__int64 queueId, std::shared_ptr<const Account> recipient, __int64 storedId, const String &fileName);

      // At the save of a copy made within the recipient's own mailbox: every
      // row for the source id gets a sibling for the copy's id, so the
      // recall follows the message wherever the reader filed it. Costs one
      // indexed select per copy.
      static void Copied(__int64 sourceId, __int64 copyId, __int64 accountId);

      // The rows for (sender, Message-ID) not older than RecallDays, oldest
      // first. False when the database did not answer.
      static bool List(__int64 senderId, const String &messageId, std::vector<Row> &rows);

      // One row as it is now. False when there is no such row, or the
      // database did not answer.
      static bool Get(__int64 rowId, Row &row);

      // The outcome, written once.
      static bool Mark(__int64 rowId, Outcome outcome, __int64 when);

      // Rows older than RecallDays go.
      static void Sweep();

      static const char *OutcomeWord(Outcome outcome);
   };
}
