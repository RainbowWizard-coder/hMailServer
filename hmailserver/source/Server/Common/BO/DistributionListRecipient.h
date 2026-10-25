// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

 #pragma once


namespace HM
{
   class DistributionListRecipient : public BusinessObject<DistributionListRecipient>
   {
   public:
      DistributionListRecipient(void);
      ~DistributionListRecipient(void);

      String GetName() const {return address_; }

      void SetAddress(const String & sAddress);
      String GetAddress(void) const {return address_;}

      __int64 GetListID() const  {return list_id_;}
      void SetListID(__int64 newVal) {list_id_ = newVal; }

      // ---- the list manager's view of a member (Util/ListManager.h) ----
      //
      // A member an administrator adds is confirmed, receives each message and
      // has no token until the manager first needs one, which is exactly what
      // every row an upgrade finds says - so a list that is not managed reads
      // these and changes nothing.

      enum Delivery
      {
         DeliveryEach = 0,     // every message, as it is distributed
         DeliveryDigest = 1,   // one digest a day
         DeliveryNone = 2      // still a member, sent nothing
      };

      enum Confirmation
      {
         Pending = 0,          // asked to join; waiting for their own confirmation
         Confirmed = 1,        // a member
         AwaitingOwner = 2,    // confirmed, waiting for the owner's approval
         OptedOut = 3          // an address the list's membership query answers,
                               // that has left: not a member, and the query's
                               // answer for it is not sent the list's mail
      };

      int GetDelivery() const {return delivery_; }
      void SetDelivery(int newVal) {delivery_ = newVal; }

      // Unix time: when they joined, or for a pending row when they asked.
      __int64 GetSubscribed() const {return subscribed_; }
      void SetSubscribed(__int64 newVal) {subscribed_ = newVal; }

      int GetConfirmed() const {return confirmed_; }
      void SetConfirmed(int newVal) {confirmed_ = newVal; }

      // True for a member the list delivers to and lets post: every row but
      // a request still waiting for a confirmation or an approval, and an
      // address that has left.
      bool GetIsMember() const {return confirmed_ == Confirmed; }

      // True for a row that stands between the membership query and the list's
      // mail for its address: a member (who is sent it as a row, with their own
      // delivery) and an address that has left (who is sent nothing).
      bool GetTakesQueryPlace() const {return confirmed_ == Confirmed || confirmed_ == OptedOut; }

      // The member's secret: 64 hex characters, random, replaced when used to
      // confirm. The confirmation link, the member's own page and the one-click
      // unsubscribe carry it.
      String GetToken() const {return token_; }
      void SetToken(const String &newVal) {token_ = newVal; }

      int GetHardBounces() const {return hard_bounces_; }
      void SetHardBounces(int newVal) {hard_bounces_ = newVal; }

      int GetSoftBounces() const {return soft_bounces_; }
      void SetSoftBounces(int newVal) {soft_bounces_ = newVal; }

      // Unix time the current soft count started; the window runs from here.
      __int64 GetSoftSince() const {return soft_since_; }
      void SetSoftSince(__int64 newVal) {soft_since_ = newVal; }

      __int64 GetLastBounce() const {return last_bounce_; }
      void SetLastBounce(__int64 newVal) {last_bounce_ = newVal; }

      // Why delivery was stopped, when the bounces stopped it; empty otherwise.
      String GetReason() const {return reason_; }
      void SetReason(const String &newVal) {reason_ = newVal; }

      bool XMLStore(XNode *pParentNode, int iOptions);
      bool XMLLoad(XNode *pParentNode, int iOptions);
      bool XMLLoadSubItems(XNode *pParentNode, int iOptions) {return true; }

   protected:

      String address_;
      __int64 list_id_;

      int delivery_;
      __int64 subscribed_;
      int confirmed_;
      String token_;
      int hard_bounces_;
      int soft_bounces_;
      __int64 soft_since_;
      __int64 last_bounce_;
      String reason_;
   };
}
