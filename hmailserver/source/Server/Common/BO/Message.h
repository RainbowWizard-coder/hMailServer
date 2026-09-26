// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <atomic>

namespace HM
{
   class MessageData;
   class MessageRecipients;

   class Message : public BusinessObject<Message>
   {
   public:

      enum State
      {
         Created = 0,
         Delivering = 1,
         Delivered = 2,
         // 3 is a message held for ETRN (ExternalDelivery.cpp), written as a
         // literal there. 4 is one kept out of the delivery queue for review
         // by the compromised-account detection (Util/CompromiseDetector.h):
         // the deliverer selects messagetype 1 alone, so it is never picked
         // up until it is released back to Delivering.
         HeldForReview = 4,
         // 5 is one held for a sandbox's verdict on its attachments
         // (AntiVirus/Sandbox.h). Kept out of the delivery queue the same way,
         // and put back to Delivering by the sandbox's poller once a verdict,
         // the timeout or an administrator has decided - never by the
         // compromised-account sweep, which ends only messagetype 4. The
         // compromised-account hold takes precedence: an account locked while
         // its message waits here has the message moved to 4, where no
         // sandbox decision reaches it.
         HeldForSandbox = 5,
      };

      enum Flags
      {
         FlagSeen = 1,
         FlagDeleted = 2,
         FlagFlagged = 4,
         FlagAnswered = 8,
         FlagDraft = 16,
         FlagRecent = 32,
         FlagVirusScan = 64,
         FlagSpam = 128,

         // RFC 3030 BINARYMIME. The ninth bit, which is why hm_messages.
         // messageflags had to be widened from tinyint to smallint in schema
         // 6025 - FlagSeen..FlagSpam had taken every bit a tinyint could hold.
         FlagBinaryMime = 256,

         // A message the quarantine released: an administrator, or the recipient
         // through the webmail or a digest link, said "this was wrong". Persisted
         // with the other flags because the release goes back through the delivery
         // queue, and the delivery that picks it up - possibly after a restart -
         // must know not to hold it again. Only the server sets it: it lives in
         // hm_messages, not in anything a sender writes, so it cannot be forged
         // the way a header could.
         FlagReleasedFromQuarantine = 512,

         // The message has had its first delivery pass. Until outbound shaping
         // existed, "first pass" and "retry count nought" were the same thing,
         // and DKIM/ARC signing, the mirror copy and a rule's policy notice were
         // gated on the counter. A shaping deferral (SMTP/DestinationBackoff.h)
         // leaves the counter at nought on purpose - nothing was attempted -
         // so those three need a mark of their own, or every held pass would
         // mirror, seal and notify again. Set in the same UPDATE that
         // reschedules an uncounted pass, so a restart cannot lose it.
         FlagFirstPassDone = 1024,

         // Released from the quarantine by a person, from an entry the SANDBOX
         // put there (AntiVirus/Sandbox.h): the one release the sandbox does not
         // examine again, since the remembered malicious verdict would put it
         // straight back. Set beside FlagReleasedFromQuarantine by
         // QuarantineStore::Release and ReleaseTo, only when the entry's reason
         // is the sandbox's own; every other release is examined like new mail.
         FlagReleasedFromSandboxQuarantine = 2048,

         // Submitted by a signed-in user of this server: an authenticated SMTP
         // session, the webmail's send (and what is sent through its code - a
         // scheduled draft, a read receipt, an unsubscribe). Set by the server
         // when it creates the message, never read from anything a sender
         // wrote. The sandbox leaves such a message alone unless
         // SandboxInternalMail says otherwise, so that what the server's own
         // users send is not uploaded to a third party by default.
         FlagSubmittedBySignedInUser = 4096,

         // The pass about to run repeats the delivery attempt the last pass
         // made: that pass ran the rules and was then held by this server's
         // own shaping (SMTP/DestinationBackoff.h) without counting a try. Set
         // with FlagFirstPassDone in the uncounted reschedule's UPDATE, cleared
         // by the counted one's. The rule actions that queue a new message - a
         // forward, a copy, a reply (SMTP/RuleApplier.cpp) - read it, so that a
         // message held through ten passes forwards once for the attempt and
         // not ten times; a counted retry clears it, and runs them as every
         // counted retry always has, for the 'delivery attempts' condition.
         FlagRepeatsAttempt = 8192
      };

      Message(bool generateFileName);
      Message(const Message& other);
      Message();
      virtual ~Message();

      void Initialize(bool generateFileName);

      String GetName() {return filename_; }

      static String GenerateFileName();

      String GetPartialFileName() const;
      void SetPartialFileName(const String &FileName);

      String GetFromAddress() const { return from_address_; }
      void SetFromAddress(const String &FromAddress) { from_address_ = FromAddress; }

      unsigned int GetUID() const { return uid_; }
      void SetUID(unsigned int  uid) { uid_ = uid; }

      // RFC 7162 (CONDSTORE/QRESYNC): per-mailbox mod-sequence value, bumped on
      // arrival and on every metadata (flag) change.
      __int64 GetModSeq() const { return message_modseq_.load(); }
      void SetModSeq(__int64 modSeq) { message_modseq_.store(modSeq); }

      __int64 GetAccountID() const { return message_account_id_; }
      void SetAccountID(__int64 MsgAccountID) { message_account_id_ = (int) MsgAccountID; }
   
      State GetState() const { return (State) message_state_; }
      void SetState(State MessageState) { message_state_ = MessageState; }

      int GetSize() const { return message_size_; }
      void SetSize(int MessageSize) { message_size_ = MessageSize; }

      __int64 GetFolderID() const { return message_folder_id_; }
      void SetFolderID(__int64 iFolderID) { message_folder_id_ = (int) iFolderID; }

      // The flags of a message in an IMAP folder's collection are shared the
      // same way as its keywords (below): STORE, FETCH's \Seen, the REST API
      // and the collection's own \Recent claim all change them from their own
      // threads. So flags_ is atomic and every one-flag setter changes its bit
      // alone - a plain "flags_ = flags_ | bit" from two threads at once could
      // put back a bit the other had just cleared, or clear one it had set.
      short GetFlags() const { return flags_.load(); }
      void SetFlags(short iNewVal) { flags_.store(iNewVal); }

      // Sets the bits in mask to their value in values and leaves every other
      // bit as it is at that moment, as one atomic step.
      void SetFlagBits(short mask, short values);

      // Held by whoever reads, changes and writes this message's flags and
      // keywords as one step (FolderManager::ChangeMessageFlags), so that two
      // such changes - two STOREs from two sessions, a STORE and the REST
      // API - cannot interleave and write back an older state over a newer
      // one, in memory or in hm_messages. Only ChangeMessageFlags takes it,
      // and with no other lock held. Under it the message's own members, the
      // database and the folder's cached mod-sequence (IMAPFolderContainer::
      // UpdateCurrentModSeq) are touched - never another message's lock, and
      // nothing that could be waiting for this one.
      boost::mutex &GetFlagChangeMutex() const { return flag_change_mutex_; }

      // IMAP keywords (RFC 3501 - the flags that are not system flags; the
      // webmail's labels): atoms, kept space-separated, compared without
      // case, at most 500 characters together. Persisted as messagekeywords
      // beside messageflags, written by the same statement.
      //
      // A message in an IMAP folder's collection is one object that every
      // session on the mailbox shares (STORE and FETCH are handed the live
      // one), so another session's thread may read these while a STORE
      // rewrites them. keywords_ is a heap-owning string, so every access goes
      // through keywords_mutex_, and AddKeyword and RemoveKeyword do their read,
      // change and write under one hold of it - two STOREs at once could
      // otherwise free the same buffer, or one lose the other's keyword.
      String GetKeywords() const;
      void SetKeywords(const String &keywords);

      // In memory only, never persisted: the id of the stored message this one
      // was copied from within the same account (PersistentMessage::
      // CopyToIMAPFolder), so that the save which assigns the copy its own id
      // can carry the message-recall rows across (Util/MessageRecall.h).
      // Cleared once the rows have been written. 0 for anything else.
      __int64 GetCopiedFromID() const { return copied_from_id_; }
      void SetCopiedFromID(__int64 id) { copied_from_id_ = id; }
      std::vector<String> GetKeywordList() const;
      void SetKeywordList(const std::vector<String> &keywords);
      bool HasKeyword(const String &keyword) const;
      // False when the keyword would not fit; the message is then unchanged.
      bool AddKeyword(const String &keyword);
      void RemoveKeyword(const String &keyword);
      static const int KeywordsMaximumLength = 500;

      bool GetFlagSeen() const;
      void SetFlagSeen(bool bNewVal);
      bool GetFlagDeleted() const;
      void SetFlagDeleted(bool bNewVal);
      bool GetFlagDraft() const;
      void SetFlagDraft(bool bNewVal);
      bool GetFlagAnswered() const;
      void SetFlagAnswered(bool bNewVal);
      bool GetFlagFlagged() const;
      void SetFlagFlagged(bool bNewVal);
      bool GetFlagRecent() const;
      void SetFlagRecent(bool bNewVal);
      bool GetFlagVirusScan() const;
      void SetFlagVirusScan(bool bNewVal);
      bool GetFlagSpam() const;
      void SetFlagSpam(bool bNewVal);

      // RFC 3030: this message's content is BINARYMIME and must never be
      // transmitted down a line-oriented DATA path. SMTPClientConnection refuses
      // to relay a message carrying this mark (554 5.6.3).
      //
      // PERSISTED, as bit 256 of flags_. It was in-memory only when it shipped,
      // because messageflags was a tinyint with all eight bits taken - and that
      // gap was real: a restart with a binary message still queued for an
      // external recipient, or a forwarded copy delivered from a cold cache, read
      // the mark back as false and relayed the content over DATA, corrupting it
      // at the first NUL and smuggling bare LFs past the check that exists to
      // stop them. Schema 6025 widened the column for this, so the mark now
      // survives the database round-trip and the refusal holds on every path.
      bool GetReleasedFromQuarantine() const { return (GetFlags() & FlagReleasedFromQuarantine) != 0; }
      void SetReleasedFromQuarantine(bool bNewVal)
      {
         SetFlagBits((short) FlagReleasedFromQuarantine, bNewVal ? (short) FlagReleasedFromQuarantine : (short) 0);
      }

      // See FlagReleasedFromSandboxQuarantine.
      bool GetReleasedFromSandboxQuarantine() const { return (GetFlags() & FlagReleasedFromSandboxQuarantine) != 0; }
      void SetReleasedFromSandboxQuarantine(bool bNewVal)
      {
         SetFlagBits((short) FlagReleasedFromSandboxQuarantine, bNewVal ? (short) FlagReleasedFromSandboxQuarantine : (short) 0);
      }

      // See FlagSubmittedBySignedInUser.
      bool GetSubmittedBySignedInUser() const { return (GetFlags() & FlagSubmittedBySignedInUser) != 0; }
      void SetSubmittedBySignedInUser(bool bNewVal)
      {
         SetFlagBits((short) FlagSubmittedBySignedInUser, bNewVal ? (short) FlagSubmittedBySignedInUser : (short) 0);
      }

      // Where the message came from, as the server itself saw it (schema 6101):
      // what a room or a piece of equipment reads before it believes that the
      // organiser an invitation names is who sent it (Util/ResourceBooking.h).
      // Set by the server where it creates the message and never read from
      // anything a sender wrote. The submitter is the account an authenticated
      // session submitted it as - SMTP AUTH, and the webmail's own sends - or
      // 0; the domains are what the inbound checks vouched for in mail from
      // outside, "dmarc=example.com" (AuthenticationResults::PassedDomains),
      // or empty. Persisted in hm_messages.messagesubmitterid and
      // messageauthdomains on a queue row only (PersistentMessage::SaveObject),
      // so that a delivery made after a restart still knows and a mailbox row
      // never carries them.
      __int64 GetSubmitterAccountID() const { return submitter_account_id_; }
      void SetSubmitterAccountID(__int64 accountId) { submitter_account_id_ = accountId; }
      String GetAuthDomains() const { return auth_domains_; }
      void SetAuthDomains(const String &domains) { auth_domains_ = domains; }

      bool GetBinaryMime() const { return (GetFlags() & FlagBinaryMime) != 0; }
      void SetBinaryMime(bool bNewVal)
      {
         SetFlagBits((short) FlagBinaryMime, bNewVal ? (short) FlagBinaryMime : (short) 0);
      }

      void SetNoOfRetries(unsigned short lNewVal) {no_of_retries_ = lNewVal; }
      unsigned short GetNoOfRetries() const { return no_of_retries_;}


      // See FlagFirstPassDone. The three "first pass only" steps of the
      // deliverer ask both this and the retry counter.
      bool GetFirstPassDone() const { return (GetFlags() & FlagFirstPassDone) != 0; }
      void SetFirstPassDone(bool done)
      {
         SetFlagBits((short) FlagFirstPassDone, done ? (short) FlagFirstPassDone : (short) 0);
      }

      // See FlagRepeatsAttempt.
      bool GetRepeatsAttempt() const { return (flags_ & FlagRepeatsAttempt) != 0; }
      void SetRepeatsAttempt(bool repeats)
      {
         if (repeats)
            flags_ |= FlagRepeatsAttempt;
         else
            flags_ &= ~FlagRepeatsAttempt;
      }

      void SetCreateTime(const String &sCreateTime) {create_time_ = sCreateTime; }
      String GetCreateTime() const {return create_time_; }

      // RFC 8514 (SAVEDATE): when the message was saved into its current
      // mailbox. Distinct from the create time, which IMAP COPY must preserve
      // as INTERNALDATE; a copy gets a fresh save date.
      void SetSaveDate(const String &sSaveDate) {save_date_ = sSaveDate; }
      String GetSaveDate() const {return save_date_; }

      // RFC 8474 (OBJECTID): the stable email id. Stamped at first save and -
      // the opposite of the save date - deliberately carried by copies: the
      // same message content keeps the same EMAILID wherever it goes.
      void SetEmailId(const String &sEmailId) {email_id_ = sEmailId; }
      String GetEmailId() const {return email_id_; }

      std::shared_ptr<MessageRecipients> GetRecipients();

      bool XMLStore(XNode *pParentNode, int iOptions);
      bool XMLLoad(XNode *pNode, int iOptions);
      bool XMLLoadSubItems(XNode *pNode, int iOptions) {return true;}

   protected:

      int message_size_;

      AnsiString create_time_;
      AnsiString save_date_;
      AnsiString email_id_;
      AnsiString filename_;
      AnsiString from_address_;
      
      int message_account_id_;
      int message_folder_id_;

      short message_state_;
      short no_of_retries_;
      // Atomic: see GetFlags.
      std::atomic<short> flags_;

      // Guarded by keywords_mutex_: see GetKeywords.
      String keywords_;
      mutable boost::mutex keywords_mutex_;

      // See GetFlagChangeMutex.
      mutable boost::mutex flag_change_mutex_;

      unsigned int uid_;

      // Atomic because a flag change writes it (PersistentMessage::SaveFlags)
      // while other sessions' FETCHes read it.
      std::atomic<__int64> message_modseq_;

      __int64 copied_from_id_ = 0;

      // See GetSubmitterAccountID.
      __int64 submitter_account_id_ = 0;
      String auth_domains_;

   private:

      static std::vector<String> SplitKeywords_(const String &keywords);
      static String JoinKeywords_(const std::vector<String> &keywords);

      bool GetFlag_(int iFlag) const;
      void SetFlag_(int iFlag, bool bSet);

      std::shared_ptr<MessageRecipients> recipients_;

   };
}
