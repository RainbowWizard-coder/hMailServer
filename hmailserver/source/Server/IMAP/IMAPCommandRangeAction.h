// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "IMAPCommand.h"
#include "IMAPFolderView.h"

namespace HM
{
   class Account;
   class IMAPFolder;
   class Message;

   // What to do when a message set names a message another session has expunged and
   // this one has not been told about yet.
   enum class MissingMessagePolicy
   {
      // Skip it. Required for the UID variants (RFC 3501 6.4.8).
      Ignore,
      // Act on the messages which do exist, then fail the command (RFC 2180 4.1.3).
      ReportAfterActing,
      // Fail without acting on any of the messages.
      FailBeforeActing,
   };

   class IMAPCommandRangeAction : public IMAPCommand
   {
      friend class IMAPSequenceSetTester;

   public:
      IMAPCommandRangeAction();
      virtual ~IMAPCommandRangeAction();

      void SetIsUID(bool bIsUID);

      IMAPResult ExecuteCommand(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<IMAPCommandArgument> pArgument) {return IMAPResult();}
      IMAPResult DoForMails(std::shared_ptr<IMAPConnection> pConnection, const String &sMailNos, const std::shared_ptr<IMAPCommandArgument> pArgument);

      // RFC 4315 (UIDPLUS): the "[COPYUID <validity> <src-set> <dst-set>] " response-code
      // prefix accumulated during COPY/MOVE, or an empty string when not applicable.
      String GetUIDPlusResponseCode();

      // RFC 7162 (CONDSTORE): the "[MODIFIED <set>] " response-code prefix produced by a
      // conditional STORE (UNCHANGEDSINCE), or an empty string when not applicable.
      virtual String GetConditionalStoreResponseCode() { return _T(""); }

      // The lessons for spamd that HoldLesson kept, queued now: what a COPY or a MOVE
      // calls once it has succeeded, its copies there to stay.
      void GiveLessons();

   protected:

      bool GetIsUID();
      virtual IMAPResult DoAction(std::shared_ptr<IMAPConnection> pConnection, int messageIndex, std::shared_ptr<Message> pMessage, const std::shared_ptr<IMAPCommandArgument> pArgument) = 0;

      // Override and return true when DoAction updates the message. Such commands are
      // given the objects the folder's collection holds rather than copies of them.
      virtual bool UsesLiveMessages() const { return false; }

      virtual MissingMessagePolicy GetMissingMessagePolicy() const { return MissingMessagePolicy::Ignore; }

      // Records a source->destination UID mapping for the UIDPLUS COPYUID response.
      void RecordCopyUid(unsigned int sourceUid, unsigned int destUid, unsigned int destUidValidity);

      // The same, for a copy or a move into folder, made only when the user may read
      // the folder: COPYUID tells its UIDVALIDITY and UIDs, and RFC 4315 section 6
      // has it not given to a user who may not SELECT or EXAMINE it - a folder one
      // may only drop mail into (upstream #650).
      void RecordCopyUidIfReadable(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<IMAPFolder> folder, unsigned int sourceUid, unsigned int destUid);

      // Forgets every mapping recorded, for a command that has taken its copies out
      // again: GetUIDPlusResponseCode is then empty.
      void ForgetCopyUids();

      // SpamAssassinLearnOnMove's lesson of a copy into or out of the Junk folder
      // (SpamAssassinLearner::IsLesson), kept until the command has succeeded
      // (GiveLessons) rather than queued as the copy is made: a COPY or a MOVE that
      // fails takes its copies out again, and a lesson queued as each copy was made
      // told spamd of a move that never happened. A command that fails forgets them
      // (ForgetLessons).
      void HoldLesson(std::shared_ptr<Message> copy, std::shared_ptr<const Account> destinationOwner, bool spam);
      void ForgetLessons();

      // What a copy or a move into folder carries (RFC 4314 section 4): the flags the
      // user may set there - \Seen with the 's' right, \Deleted with 't', and \Draft,
      // \Answered, \Flagged and the keywords with 'w' - and not the others, which
      // were copied whatever the rights but for \Seen (upstream #666).
      static void KeepOnlySettableFlags(std::shared_ptr<IMAPConnection> pConnection, std::shared_ptr<IMAPFolder> folder, std::shared_ptr<Message> message);

   private:

      // Translates a message set into this session's messages, as (sequence number,
      // entry) pairs, against the session's own view of the folder: each message
      // once, in the order the set first names it.
      std::vector<std::pair<int, IMAPViewEntry> > ResolveTargets_(std::shared_ptr<IMAPFolderView> view, const String &sMailNos);

      String JoinUids_(const std::vector<unsigned int> &uids);

      bool is_uid_;

      std::vector<unsigned int> uidplus_source_uids_;
      std::vector<unsigned int> uidplus_dest_uids_;
      unsigned int uidplus_dest_uidvalidity_;

      struct Lesson_
      {
         std::shared_ptr<Message> copy;
         std::shared_ptr<const Account> destination_owner;
         bool spam = false;
      };

      std::vector<Lesson_> lessons_;

   };


   // A message set held to the messages 6.3.5 resolved it to, each once - its
   // code kept here - over generated views and sets, by UID and by sequence
   // number, the view's UID lookups held to reading every entry, and both to
   // the time they take: 50,000 UIDs over 50,000 messages, each found by
   // halving. And IMAPListLookup's: a SEARCH key's set read into runs, held to
   // IsItemInList; a UID set's ranges merged, held to reading every range; "$"
   // standing in once, where it stands alone; with their times. Run by
   // ClassTester on Windows and by imap-parse-tests on Linux.
   class IMAPSequenceSetTester
   {
   public:
      void Test(int cases = 20000);
   };
}
