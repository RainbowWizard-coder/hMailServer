// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../../Threading/Task.h"

namespace HM
{
   class IMAPFolder;
   class Message;
   class Account;

   // SpamAssassinLearnOnMove: a message a user moves into their Junk folder is
   // spam, and one they move out of it is ham, and spamd is told so (the spamc
   // TELL command, "Set: local", the same Bayes store its verdicts come from).
   // This is the feedback loop every other mail system has and this one did not:
   // until now the only way to teach spamd was sa-learn on the server's console.
   //
   // Off by default, like the other spamd extras: it sends a command an existing
   // spamd may be configured to refuse (--allow-tell is spamd's own switch), and an
   // operator turns it on knowing that.
   class SpamAssassinLearner
   {
   public:
      // Called after an IMAP COPY or MOVE has written the copy: decides whether it
      // was a move into or out of a \Junk folder of the destination owner's own
      // tree, and if so queues the lesson. Never blocks the IMAP command: the
      // conversation with spamd runs on the asynchronous work queue.
      static void LearnFromMove(std::shared_ptr<IMAPFolder> source, std::shared_ptr<IMAPFolder> destination,
                                std::shared_ptr<Message> copy, std::shared_ptr<const Account> destinationOwner);

      // LearnFromMove in its two halves, for an IMAP COPY or MOVE, which asks the
      // first as it makes each copy and gives the second only once the command has
      // succeeded: one that fails takes its copies out again (IMAPCopy::RollBack,
      // IMAPMove::RollBack), and a lesson queued for one of them told spamd of a
      // move that never happened. IsLesson says whether a copy from source into
      // destination is a lesson, and which (spam true into Junk, false out of it);
      // Teach queues it for the copy.
      static bool IsLesson(std::shared_ptr<IMAPFolder> source, std::shared_ptr<IMAPFolder> destination,
                           std::shared_ptr<const Account> destinationOwner, bool &spam);
      static void Teach(std::shared_ptr<Message> copy, std::shared_ptr<const Account> destinationOwner, bool spam);

      // The conversation itself, synchronous: connect, TELL, wait for the reply.
      // Returns whether spamd acknowledged (EX_OK). Public for the task below.
      static bool Tell(const String &messageFile, bool spam, const String &user);
   };

   class SpamAssassinLearnTask : public Task
   {
   public:
      // removeFileAfterwards: the file is a copy made for this lesson alone (a
      // spam trap's is, because the message it was taken from is on its way to a
      // mailbox), and nothing will look at it again once spamd has had it.
      SpamAssassinLearnTask(const String &messageFile, bool spam, const String &user, bool removeFileAfterwards = false);
      virtual void DoWork();

   private:
      String message_file_;
      bool spam_;
      String user_;
      bool remove_file_afterwards_;
   };
}
