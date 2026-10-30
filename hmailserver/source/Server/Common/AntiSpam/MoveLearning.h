// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <map>

namespace HM
{
   class Account;
   class IMAPConnection;
   class IMAPFolder;
   class Message;

   // LEARNING FROM THE JUNK FOLDER: a message a user moves into their Junk folder
   // is spam, one they move out of it is ham, and the spam filter is told so - by
   // the backend the administrator chose (MoveLearningBackend):
   //
   //   SpamAssassin (0, the default) - spamd's TELL, as SpamAssassinLearnOnMove has
   //     always done it (SpamAssassin/SpamAssassinLearner.h), switched on by that
   //     [Settings] key and unchanged in every request it makes;
   //   Rspamd (1) - the controller's /learnspam and /learnham (RspamdLearner.h).
   //
   // One rule decides, for both, which moves are lessons, and this is where it is
   // (IsLesson); the IMAP COPY and MOVE commands ask it as they copy each message
   // and hand the lessons on only once the command has succeeded
   // (IMAP/IMAPMoveDispatch.h), and the webmail's move asks it after its move.
   //
   // A lesson is a move between two of one account's own folders, into its Junk
   // folder (spam) or out of it (ham). Nothing between two folders neither of
   // which is Junk, nothing from Junk to Junk, and nothing for a delete or an
   // expunge, which moves nothing anywhere. No public folder. Which folder is the
   // Junk folder is what LIST says (IMAPSpecialUse::Resolve, GitLab work item
   // #108's question of a "Junk" beside a "Spam" answered in docs/JunkFolderLearning.md).
   //
   // A COPY or a UID COPY into or out of Junk teaches as a MOVE does while
   // MoveLearningFromCopy is on, as it is by default and as it always did; off,
   // only a MOVE, a UID MOVE and the webmail's move teach. A COPY, a STORE of
   // \Deleted and an EXPUNGE are three commands, and nothing here joins them
   // into a move.
   //
   // Whose lesson it is. For Rspamd, the account signed in to the session must
   // own both folders: a move in another account's mailbox, shared or delegated
   // to this one, teaches nobody, since the judgement is the delegate's and the
   // statistics would be the owner's. The IMAP master user opening an account's
   // mailbox is signed in to that account, and its moves are that account's. For
   // SpamAssassin the rule is the one it has had since it came, so that nothing
   // changes for a server that does not switch backends: both folders one
   // account's, and the lesson that account's, whoever moved the message.
   class MoveLearning
   {
   public:
      enum Backend
      {
         BackendSpamAssassin = 0,
         BackendRspamd = 1
      };

      // What decides whether one copy is a lesson, as plain values.
      class Facts
      {
      public:
         Facts();

         // The backend chosen, and whether it is switched on: for SpamAssassin
         // SpamAssassinLearnOnMove and SpamAssassin itself, for Rspamd the choice
         // of it alone (its address and classifier are checked when a lesson is
         // sent, where a missing one is reported).
         int backend;
         bool spamassassin_learn_on_move;
         bool spamassassin_enabled;

         // A COPY or a UID COPY, and whether MoveLearningFromCopy lets one teach.
         bool is_copy;
         bool learn_from_copy;

         // Whether the destination's owner was resolved.
         bool has_owner;

         // The account signed in to the session, and the two folders' accounts
         // (0 for a public folder) and kinds.
         __int64 session_account_id;
         __int64 source_account_id;
         __int64 destination_account_id;
         bool source_public;
         bool destination_public;

         // Whether each folder is the account's Junk folder, as LIST says.
         bool source_junk;
         bool destination_junk;
      };

      // The first half of the rule, which needs no folder tree: whether the backend
      // is on, the command may teach, and the folders are an account's own. Asked
      // before the Junk folder is looked for, so that a server that teaches
      // nothing does no more on a COPY or a MOVE than read its settings.
      static bool MayTeach(const Facts &facts);

      // The whole rule: MayTeach, and a move into Junk (spam true) or out of it
      // (spam false).
      static bool IsLesson(const Facts &facts, bool &spam);

      // The rule asked for a copy an IMAP COPY (isCopy) or MOVE has made from the
      // session's selected folder into destination: reads the settings, and the
      // owner's folder designations only when MayTeach holds. backend says which
      // backend the lesson is for, so that it goes to the one chosen when the copy
      // was made.
      static bool IsLesson(std::shared_ptr<IMAPConnection> connection, std::shared_ptr<IMAPFolder> source,
                           std::shared_ptr<IMAPFolder> destination, std::shared_ptr<const Account> destinationOwner,
                           bool isCopy, bool &spam, int &backend);

      // For a move made and committed elsewhere than in an IMAP command - the
      // webmail's: decides, as for a MOVE made by sessionAccount, and queues the
      // lesson at once.
      static void LearnFromMove(std::shared_ptr<IMAPFolder> source, std::shared_ptr<IMAPFolder> destination,
                                std::shared_ptr<Message> copy, std::shared_ptr<const Account> destinationOwner,
                                std::shared_ptr<const Account> sessionAccount);

      // Queues one lesson for the backend it was decided for. Never blocks: each
      // backend teaches on the asynchronous work queue.
      static void Teach(int backend, std::shared_ptr<Message> copy, std::shared_ptr<const Account> destinationOwner, bool spam);

   private:
      static bool IsJunk_(const std::map<__int64, int> &designations, __int64 folderId);
   };
}
