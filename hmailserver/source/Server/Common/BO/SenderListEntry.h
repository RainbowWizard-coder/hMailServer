// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // One row of the sender list: an address or a domain that somebody has said
   // is wanted, or is not, together with who said it and until when.
   //
   // WHY ONE TABLE AND NOT THREE LISTS
   //
   // Three questions were asked separately - a reader's safe senders, a
   // domain's allow and block lists, the server's - and they are the same
   // question at three distances: who decides that a message is wanted. So they
   // are one row shape with a scope on it, one matching rule (SenderPattern),
   // one precedence (SenderListCache), one REST shape and one editor. The
   // alternative was three tables that would drift, and three places to look
   // when a message went the wrong way.
   //
   // WHY IT IS NOT hm_blocked_senders
   //
   // That list already exists and is untouched, because it answers a different
   // question at a different moment: it runs during the SMTP conversation,
   // against the envelope sender, and adds a score that can refuse the message
   // for EVERY recipient at once. A conversation shared by several recipients
   // cannot be refused for one of them, so nothing per-recipient can live
   // there. This list is read at local delivery, where the account and its
   // domain are both known, and it decides what ONE recipient sees.
   class SenderListEntry : public BusinessObject<SenderListEntry>
   {
   public:
      SenderListEntry();
      ~SenderListEntry();

      enum Scope
      {
         ScopeServer = 0,
         ScopeDomain = 1,
         ScopeAccount = 2
      };

      enum Kind
      {
         KindAllow = 0,
         KindBlock = 1
      };

      String GetName() const { return value_; }

      int GetScope() const { return scope_; }
      void SetScope(int newVal) { scope_ = newVal; }

      __int64 GetOwnerID() const { return owner_id_; }
      void SetOwnerID(__int64 newVal) { owner_id_ = newVal; }

      int GetKind() const { return kind_; }
      void SetKind(int newVal) { kind_ = newVal; }

      bool GetBlocked() const { return kind_ == KindBlock; }
      void SetBlocked(bool newVal) { kind_ = newVal ? KindBlock : KindAllow; }

      String GetValue() const { return value_; }

      // Canonicalised on the way in - trimmed, lower-cased, a leading '@'
      // dropped - so that "@Example.COM " and "example.com" are one row, and so
      // that every reader of this object compares the same text.
      void SetValue(const String &newVal);

      String GetReason() const { return reason_; }
      void SetReason(const String &newVal) { reason_ = newVal; }

      bool GetExpires() const { return expires_ ? true : false; }
      void SetExpires(bool newVal) { expires_ = newVal ? 1 : 0; }

      DateTime GetExpiresTime() const { return expires_time_; }
      void SetExpiresTime(DateTime newVal) { expires_time_ = newVal; }

      DateTime GetCreated() const { return created_; }
      void SetCreated(DateTime newVal) { created_ = newVal; }

      // True when this entry covers the given envelope sender. The matching
      // rule is SenderPattern's, shared with the server-wide blocked senders.
      bool Matches(const String &address) const;

      // How specific this entry is, for the tie-break between two entries of
      // the same scope that both match. See SenderPattern::Specificity.
      int Specificity() const;

      // True when the entry has an expiry and it has passed. An expired entry
      // is treated as absent by everything that reads the list, and is swept
      // out of the table by the digest task's expiry pass - so a list that the
      // sweep cannot reach still behaves correctly, rather than keeping a
      // sender allowed for a year because one DELETE failed.
      bool HasExpired() const;

      bool XMLStore(XNode *pNode, int iOptions);
      bool XMLLoad(XNode *pNode, int iOptions);
      bool XMLLoadSubItems(XNode *pNode, int iOptions) { return true; }

   private:

      int scope_;
      __int64 owner_id_;
      int kind_;
      String value_;
      String reason_;
      int expires_;
      DateTime expires_time_;
      DateTime created_;
   };
}
