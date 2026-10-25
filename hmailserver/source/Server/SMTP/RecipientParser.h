// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class MessageRecipients;
   class MessageRecipient;
   class DistributionList;
   class Account;

   class RecipientParser  
   {
   public:
      RecipientParser();
      virtual ~RecipientParser();

      enum DeliveryPossibility
      {
         DP_Possible = 0,
         DP_RecipientUnknown = 1,
         DP_PermissionDenied = 2,

         // The recipient exists and is willing, but its mailbox is already at or
         // over quota. Separate from the two refusals above because it is
         // TEMPORARY - the mailbox will very likely be emptied - and answering it
         // as a permanent failure would throw away mail a retry would deliver.
         DP_MailboxFull = 3,
      };

      // checkRecipientQuota asks the one extra question RCPT TO needs and nothing
      // else does: is this mailbox already full? It is opt-in rather than always on
      // because the forwarding callers treat every answer other than DP_Possible as
      // "abandon the forward" and log it - so a full forwarding target would become
      // a dropped message instead of a deferred one, which is the opposite of what
      // this check is for.
      DeliveryPossibility CheckDeliveryPossibility(bool bSenderIsAuthed, String sSender, const String &sOriginalRecipient, String &sErrMsg, bool &bTreatSecurityAsLocal, int iRecursionLevel, bool checkRecipientQuota = false);
      void CreateMessageRecipientList(const String &sRecipientAddress, std::shared_ptr<MessageRecipients> pRecipients, bool &recipientOK);

      // Whether the last CheckDeliveryPossibility called with iRecursionLevel 0 found
      // an SRS address this server handed out and followed it back for a sender of
      // its own - not a bounce's null one - so that it is mail for an outside address
      // (bTreatSecurityAsLocal false). SMTPConnection::ProtocolRCPT_ answers such a
      // RCPT TO, named first in its transaction, as it always did when the relay rules
      // refuse it, for a server that verifies the address with a sender of its own,
      // and refuses its DATA.
      bool GetReversedSrsForSender() const { return reversed_srs_for_sender_; }

      // An address routed to a URL (Util/InboundWebhooks.h) is, by default, one
      // recipient of its own with no account behind it. An address whose route
      // is one of these - the routes a message has been taken by already - is
      // expanded as though it had no route; a route that is not one of them
      // still makes a recipient of its own.
      void SetRoutesTaken(const std::set<__int64> &routeIds) { routes_taken_ = routeIds; }

   private:

      void CreateMessageRecipientList_(const String &recipientAddress, const String &sOriginalAddress, long lRecurse, std::shared_ptr<MessageRecipients> pRecipients, bool &recipientOK);
      void AddRecipient_(std::shared_ptr<MessageRecipients> pRecipients, std::shared_ptr<MessageRecipient> pRecipient);

      // What a list's membership query answers, resolved at most once for the
      // transaction in hand. DistributionListMembership caches nothing - that
      // is the whole point of a query list, there is no membership table to
      // drift - but SMTPConnection calls this parser once per RCPT TO on one
      // connection, so without this an unauthenticated sender could repeat
      // RCPT TO:<all@example.com> against a public list and buy a full scan of
      // hm_accounts with every command, each answered 250. unavailable is
      // DistributionListMembership::Members's: the query could not be answered
      // and the caller must refuse the recipient rather than expand what came
      // back.
      std::vector<String> MembersOf_(std::shared_ptr<const DistributionList> list, std::shared_ptr<MessageRecipients> pRecipients, bool &unavailable);

      // Drops both memos below when the collection being filled is not the one
      // they were built for. The MessageRecipients of a transaction is that
      // transaction's own object, so a new MAIL FROM (or any other caller with
      // a collection of its own) is exactly a different pointer here.
      void ForgetIfNewTransaction_(std::shared_ptr<MessageRecipients> pRecipients);

      bool IsMailboxFull_(std::shared_ptr<const Account> account, String &sErrMsg);
      DeliveryPossibility UserCanSendToList_(const String &sSender, bool bSenderIsAuthenticated, std::shared_ptr<const DistributionList> pList, String &sErrMsg, int iRecursionLevel);

      // Distribution lists whose mode refused THIS transaction's sender but which
      // have a moderator, keyed by the list's primary address, lowercased. Written
      // by CheckDeliveryPossibility, read by CreateMessageRecipientList_, and the
      // reason both are methods of one object: SMTPConnection calls them back to
      // back on its one recipientParser_ member for every RCPT TO, so a mark made
      // while deciding an address is still current when that same address is
      // expanded a moment later. The check both marks and UNMARKS - a sender the
      // list accepts erases any mark a previous message on this connection left -
      // so a mark can never outlive the decision it recorded. Every other caller
      // of CreateMessageRecipientList constructs a fresh parser, whose empty set
      // means what it always meant: expand the list to its members.
      std::set<String> moderated_list_addresses_;

      // GetReversedSrsForSender's answer: set by the outermost CheckDeliveryPossibility
      // alone, so that the calls it makes for a list's members neither clear nor set it.
      bool reversed_srs_for_sender_;

      // The collection the two memos below describe, held weakly so that a
      // collection which has been freed can never be mistaken for the one in
      // hand by a later allocation at the same address: an expired weak pointer
      // locks to null, which is not the pointer being asked about.
      std::weak_ptr<MessageRecipients> memo_recipients_;

      // A list's address, lower-cased, against what its membership query
      // answered for this transaction.
      std::map<String, std::vector<String> > membership_memo_;

      // The addresses already in that collection, lower-cased, and how many
      // entries it had when this was last true of it. AddRecipient_ has
      // deduplicated by walking the whole vector on every insert since long
      // before queries existed - fine for a list somebody typed, quadratic for
      // one that answers a domain of thousands - and this is the same decision
      // taken in one lookup. Rebuilt whenever the count disagrees, so anything
      // that changes the collection behind this parser's back is noticed rather
      // than believed.
      std::set<String> memo_added_;
      size_t memo_added_count_;

      // Set by InboundWebhooks alone, on a parser of its own, when it expands
      // the address of a route that delivers as well as posts: the address as
      // it would be with no route, so that the mailbox's own recipients come
      // back rather than the route's recipient again - and a route the message
      // has been posted to is never reached a second time through an alias or
      // a list. Empty on every other parser.
      std::set<__int64> routes_taken_;

  };
}