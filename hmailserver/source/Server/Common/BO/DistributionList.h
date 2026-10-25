// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{

   class DistributionListRecipients;
   
   class DistributionList : public BusinessObject<DistributionList>
   {
   public:
      DistributionList(void);
      ~DistributionList(void);

      enum ListMode
      {
         LMPublic = 0,
         LMMembership = 1,
         LMAnnouncement = 2,
         LMDomainMembers = 3,
      };

      String GetName() {return address_; }

      __int64 GetDomainID() const {return domain_id_;}
      void SetDomainID(__int64 newVal) {domain_id_ = newVal;}

      String GetAddress() const {return address_;}
      void SetAddress(const String & sNewVal) {address_ = sNewVal;}

      bool GetActive() const {return enabled_;}
      void SetActive(bool bNewVal) {enabled_ = bNewVal;}

      bool GetRequireAuth() const {return require_auth_;}
      void SetRequireAuth(bool bNewVal) {require_auth_ = bNewVal;}

      String GetRequireAddress() const {return require_address_;}
      void SetRequireAddress(const String & sNewVal) {require_address_ = sNewVal;}

      // The list's moderator. Empty means moderation is off, which is the previous
      // behaviour byte for byte: a sender the list's mode refuses gets a 550 at
      // RCPT TO. When set, such a posting is accepted and forwarded to this address
      // instead of being distributed, and the moderator approves it by resending it
      // to the list from an AUTHENTICATED session - an authenticated sender whose
      // address is the moderator's may always post. The authentication requirement
      // is the security boundary: MAIL FROM is free text, so without it anyone who
      // could spell the moderator's address could approve their own posting.
      String GetModeratorAddress() const {return moderator_address_;}
      void SetModeratorAddress(const String & sNewVal) {moderator_address_ = sNewVal;}

      // The envelope sender (MAIL FROM / Return-Path) given to every copy this
      // list sends: distributed copies to members, and moderation forwards to the
      // moderator. Empty means the previous behaviour - copies keep the original
      // poster's envelope sender, so a dead subscriber's bounces hammer whoever
      // happened to post last. Set it to the list owner's mailbox and the bounces
      // go to the one person who can act on them.
      String GetBounceAddress() const {return bounce_address_;}
      void SetBounceAddress(const String & sNewVal) {bounce_address_ = sNewVal;}

      // The membership query (DistributionListMembership.h): empty for a
      // static list, whose members are its rows and nothing else; domain,
      // domain:<name>, pattern:<wildcard address> or everyone for a list whose
      // members are found among the server's accounts when a message is sent
      // to it, in union with its rows. Judged at save time by
      // PreSaveLimitationsCheck, so a text that is none of those never reaches
      // the column.
      String GetMembership() const {return membership_;}
      void SetMembership(const String & sNewVal) {membership_ = sNewVal;}

      ListMode GetListMode() const {return list_mode_; }
      void SetListMode(ListMode m) {list_mode_ = m; }

      // ---- the list manager (Util/ListManager.h, docs/MailingLists.md) ----
      //
      // Off on every list an upgrade finds and on every list made without
      // asking for it, and then everything below is inert: the list expands
      // to its members at RCPT TO exactly as it always has. On, the list
      // answers its own addresses (name-subscribe@, name-unsubscribe@,
      // name-request@, name-owner@, name-bounces@), receives a posting as one
      // recipient of its own and sends every member a copy of their own from
      // the delivery path.

      enum SubscribePolicy
      {
         SubscribeClosed = 0,     // members are added by the administrator only
         SubscribeConfirm = 1,    // anyone joins, and confirms by mail or page
         SubscribeModerated = 2   // anyone asks, confirms, and the owner approves
      };

      enum ArchiveMode
      {
         ArchiveNone = 0,
         ArchiveMembers = 1,
         ArchivePublic = 2
      };

      bool GetManaged() const {return managed_; }
      void SetManaged(bool newVal) {managed_ = newVal; }

      int GetSubscribePolicy() const {return subscribe_policy_; }
      void SetSubscribePolicy(int newVal) {subscribe_policy_ = newVal; }

      int GetArchive() const {return archive_; }
      void SetArchive(int newVal) {archive_ = newVal; }

      // Days an archived message is kept; 0 keeps it for as long as the list
      // exists.
      int GetArchiveDays() const {return archive_days_; }
      void SetArchiveDays(int newVal) {archive_days_ = newVal; }

      bool GetDigest() const {return digest_; }
      void SetDigest(bool newVal) {digest_ = newVal; }

      // The local hour, 0 to 23, the daily digest goes at.
      int GetDigestHour() const {return digest_hour_; }
      void SetDigestHour(int newVal) {digest_hour_ = newVal; }

      // Where the last digest stopped: the highest hm_listarchive id it held,
      // and when it was sent (Unix time). Written by the digest alone.
      __int64 GetDigestLastId() const {return digest_last_id_; }
      void SetDigestLastId(__int64 newVal) {digest_last_id_ = newVal; }
      __int64 GetDigestLastTime() const {return digest_last_time_; }
      void SetDigestLastTime(__int64 newVal) {digest_last_time_ = newVal; }

      // True when replies go to the list (Reply-To is the list's address),
      // false when they go to whoever wrote the posting.
      bool GetReplyToList() const {return reply_to_list_; }
      void SetReplyToList(bool newVal) {reply_to_list_ = newVal; }

      String GetSubjectPrefix() const {return subject_prefix_; }
      void SetSubjectPrefix(const String &newVal) {subject_prefix_ = newVal; }

      String GetFooter() const {return footer_; }
      void SetFooter(const String &newVal) {footer_ = newVal; }

      // True when each member's copy is sent from name-bounces+local=domain@,
      // so a bounce names the member whatever the report says.
      bool GetVerp() const {return verp_; }
      void SetVerp(bool newVal) {verp_ = newVal; }

      // The person who runs the list: approvals, bounce notices and
      // name-owner@ go here. Empty means the moderator's address.
      String GetOwner() const {return owner_; }
      void SetOwner(const String &newVal) {owner_ = newVal; }

      // The list's own secret: 32 random bytes as 64 hexadecimal characters,
      // made the first time the list needs it (ListManager::EnsureListSecret)
      // and never shown anywhere. The link in the copies sent to the accounts
      // the membership query answers - who have no row, and so no token of
      // their own - is an HMAC of their address under it. Written by the
      // manager alone; a whole-list save leaves it as it is.
      String GetSecret() const {return secret_; }
      void SetSecret(const String &newVal) {secret_ = newVal; }

      // The longest subject prefix and footer the columns hold.
      static const int MaxSubjectPrefixLength = 64;
      static const int MaxFooterLength = 2000;

      // Judges the manager's fields: every value in its range, the texts
      // within their columns, and an owner that is one e-mail address and
      // neither the list itself nor one of the addresses it answers. The
      // sentence is the one a save answers with, over COM and REST alike;
      // COM's own assignments and REST's reading of a body refuse an
      // out-of-range number earlier, in words of their own.
      bool ValidateManager(String &reason) const;

      // True when only the single authorized address is allowed to post. RFC 2369
      // requires "List-Post: NO" for such a list rather than the list address,
      // since telling a subscriber to reply to the list would be telling them to
      // do something the server will refuse.
      bool IsAnnouncementOnly() const {return list_mode_ == LMAnnouncement; }

      // The RFC 2919 List-Id value, angle brackets included, derived from the list
      // address: list@example.com becomes <list.example.com>. Returns an empty
      // string when the list address has no local part or no domain, in which case
      // no valid identifier can be formed.
      String GetRfc2919ListId() const;

      bool XMLStore(XNode *pParentNode, int iOptions);
      bool XMLLoad(XNode *pParentNode, int iRestoreOptions);
      bool XMLLoadSubItems(XNode *pParentNode, int iRestoreOptions);

      std::shared_ptr<DistributionListRecipients> GetMembers() const;

      size_t GetEstimatedCachingSize();

   protected:

      String address_;
      __int64 domain_id_;
      bool enabled_;

      bool require_auth_;
      String require_address_;

      String moderator_address_;
      String bounce_address_;
      String membership_;

      ListMode list_mode_;

      bool managed_;
      int subscribe_policy_;
      int archive_;
      int archive_days_;
      bool digest_;
      int digest_hour_;
      __int64 digest_last_id_;
      __int64 digest_last_time_;
      bool reply_to_list_;
      String subject_prefix_;
      String footer_;
      bool verp_;
      String owner_;
      String secret_;
   };
}
