// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The list manager: a distribution list with its manager switched on is a
// mailing list a person joins and leaves by mail and by page, with its own
// addresses, a copy of every posting for each member carrying that member's
// own way out, an archive, a daily digest and bounce handling. What replaces
// Mailman for a small organisation. docs/MailingLists.md is the reader's
// account of it; this is the code's.
//
// THE ADDRESSES A MANAGED LIST ANSWERS
//
// For a managed list name@domain, RecipientParser accepts at RCPT TO, and
// before any plus-addressing is applied:
//
//    name-subscribe@domain     ask to join, or confirm (a reply carrying the
//                              token the confirmation was sent with)
//    name-unsubscribe@domain   ask to leave: the envelope sender is sent a
//                              link and a token, and leaves when it follows
//                              the one or replies with the other
//    name-request@domain       the help text
//    name-owner@domain         the owner: expanded at RCPT TO like an alias
//    name-bounces@domain       delivery reports for the list's copies
//    name-bounces+local=domain@domain
//                              the same, with the member encoded (VERP)
//
// None of them is an account and none of them keeps anything: each is one
// recipient of the message with no account behind it, and TakeListRecipients
// answers it in the delivery path, where the moderation addresses have always
// been answered, and takes it off the message. An account, an alias or a list
// of exactly that address wins over the manager, as an administrator's address
// always wins over a derived one.
//
// A POSTING
//
// A posting to a managed list is one recipient too - the list's own address -
// rather than the members it used to be expanded into at RCPT TO, because a
// managed list sends every member a copy of their own: the member's way out is
// in its List-Unsubscribe link and its RFC 8058 one-click, and with VERP the
// member is in its envelope sender. Distribute_ writes the list's copy once
// (the subject prefix, Reply-To, the footer), archives it when the archive or
// the digest wants it, and queues one message per member who takes each
// message, signed with the list domain's DKIM key when that domain signs. Who
// may post is decided at RCPT TO exactly as for any list: the mode, the
// authentication requirement and the moderator. A posting that could not be
// read stays in the queue and is tried again, and its sender is told only when
// the retries run out.
//
// TOKENS
//
// 32 random bytes as 64 hex characters, one per member row, replaced when it is
// used to confirm. The confirmation link, the member's own page and the
// one-click unsubscribe carry it; an administrator's member has none until the
// manager first sends it a copy. A token names one row of one list, so it
// cannot act on another list. An account the list's membership query answers
// has no row, and its copies carry instead an HMAC of its address under the
// list's own secret, with the address and the list in the link: enough for
// the page to leave, never to change anything else.
//
// BOUNCES
//
// Anybody can write to name-bounces@. What stops a forged report pausing a
// member is the member tag: every copy carries X-hMailServer-List-Member, a
// SHA-256 of the member's token and the list's address, and a report counts
// only when that tag is somewhere in it - which a real report is, since RFC
// 3464 returns the failed message's header (this server's own reports do). A
// report without it, or naming somebody who is not a member, is logged and
// dropped; so is a report that says the message was delayed, delivered or
// relayed. A 5.x.x status is a hard bounce, a 4.x.x or a report whose status
// cannot be read a soft one; ListHardBouncesToUnsubscribe hard bounces, or
// ListSoftBouncesToUnsubscribe soft ones within ListBounceWindowDays, set the
// member's delivery to none with the reason recorded and the owner told once.
// The member stays a member: bounces never unsubscribe anybody.

#pragma once

namespace HM
{
   class Message;
   class MessageRecipient;
   class DeliveryFailure;
   class DistributionList;
   class DistributionListRecipient;
   class IPAddress;

   class ListManager
   {
   public:
      enum Command
      {
         CommandNone = 0,
         CommandPost = 1,          // name@domain itself
         CommandSubscribe = 2,
         CommandUnsubscribe = 3,
         CommandRequest = 4,
         CommandOwner = 5,
         CommandBounces = 6
      };

      // ---- addresses --------------------------------------------------------

      // Whether an address is one a managed, active list answers: the list's
      // own address (CommandPost), or one of its command addresses, the VERP
      // form of name-bounces@ decoded into verpMember. The address is taken as
      // the RCPT gave it, domain aliases applied, and nothing is recognised
      // when an account, an alias or a list of exactly that address exists.
      static bool ParseAddress(const String &address, Command &command, std::shared_ptr<const DistributionList> &list, String &verpMember);

      // The command addresses alone, never the list's own: what RecipientParser
      // asks at RCPT TO before plus addressing, so that a domain whose plus
      // character is '-' cannot read name-request@ as name@ with a tag. An
      // address that ends in none of the five words costs a string test.
      static bool ParseCommandAddress(const String &address, Command &command, std::shared_ptr<const DistributionList> &list, String &verpMember);

      // Whether an address is the list's own or one of its command addresses,
      // read from the two strings alone.
      static bool IsListOwnAddress(const String &address, const String &listAddress);

      // name-<command>@domain for a list address. CommandPost answers the
      // list's own address.
      static String CommandAddress(const String &listAddress, Command command);

      // name-bounces+local=domain@listdomain for a member.
      static String VerpAddress(const String &listAddress, const String &member);

      // Who runs the list: its owner, else its moderator, else the domain's
      // catch-all, else postmaster@ the list's domain.
      static String OwnerOf(std::shared_ptr<const DistributionList> list);

      // ---- the delivery path ------------------------------------------------

      // Answers every recipient of the message that is a managed list or one of
      // its addresses, and takes each off the message (and out of the
      // database), so that local and external delivery never see them. Called
      // by SMTPDeliverer::DeliverMessage before local delivery. Returns at once,
      // reading nothing, when no list is managed.
      //
      // A posting the manager could not read - its file could not be opened or
      // loaded, or its list copy could not be written - is not answered: its
      // recipient is put in held, stays in the database and, for now, in the
      // message, so that local delivery never reuses the message's file for a
      // single local recipient beside it. SetAside takes the held recipients off
      // the message after local delivery, and HoldForRetry reschedules it.
      //
      // notMine, when given, is recipients another part of the delivery path
      // is holding the same way - an inbound webhook's whose copy could not be
      // kept (InboundWebhooks.h) - which have the same shape as the manager's
      // and are left alone.
      static void TakeListRecipients(std::shared_ptr<Message> message, std::vector<std::shared_ptr<MessageRecipient> > &held,
                                     const std::vector<std::shared_ptr<MessageRecipient> > *notMine = nullptr);

      // Takes the held recipients off the message, leaving their rows.
      static void SetAside(std::shared_ptr<Message> message, const std::vector<std::shared_ptr<MessageRecipient> > &held);

      // The held postings: when the message is already rescheduled (an
      // external recipient's deferral) they go with it; otherwise the message
      // is rescheduled here, on the SMTP retry schedule, while it has retries
      // left, and when it has none each held list address is reported to the
      // sender as a class 4 failure (4.3.0, the mail system - this server's -
      // not the recipient) and its row removed. True when the message stays
      // in the queue.
      static bool HoldForRetry(std::shared_ptr<Message> message, const std::vector<std::shared_ptr<MessageRecipient> > &held,
                               bool alreadyRescheduled, std::vector<DeliveryFailure> &failures);

      // Whether any list on the server is managed, asked of the database at
      // most once a minute - and at once after a list is saved or deleted,
      // which calls ForgetManagedState. False is what keeps the delivery path
      // free on a server with no managed list.
      static bool AnyManagedList();
      static void ForgetManagedState();

      // ---- settings (hm_settings) --------------------------------------------

      // ListBaseUrl without a trailing slash, or empty when it is unset or not
      // an absolute http(s) URL of printable ASCII.
      static String BaseUrl();

      // Whether a base URL may be stored as ListBaseUrl: what BaseUrl() would
      // answer it with, and nothing it would quietly drop. reason is the
      // sentence the settings route refuses with.
      static bool IsUsableBaseUrl(const String &url, String &reason);

      static int HardBouncesToUnsubscribe();
      static int SoftBouncesToUnsubscribe();
      static int BounceWindowDays();

      // ---- tokens -----------------------------------------------------------

      // A new 64-hex-character token, or empty when no entropy was available.
      static String NewToken();

      // Only the characters a token is made of, exactly 64 of them.
      static bool IsTokenText(const String &token);

      // The tag every copy for this member carries in X-hMailServer-List-Member.
      static AnsiString MemberTag(const String &token, const String &listAddress);

      // The list's secret, made and stored the first time it is needed; empty
      // when it could be neither read nor made.
      static String EnsureListSecret(std::shared_ptr<const DistributionList> list);

      // The token in the link of a copy to an account the membership query
      // answers: HMAC-SHA256 of its lower-cased address under the list's
      // secret, as 64 hex characters. Empty without a secret.
      static String QueryMemberToken(const String &secret, const String &address);

      // ---- lists and members ------------------------------------------------

      static std::shared_ptr<DistributionList> ListById(__int64 listId);
      static std::shared_ptr<DistributionList> ListByAddress(const String &address);

      // The member row for an address in a list, or null.
      static std::shared_ptr<DistributionListRecipient> MemberOf(__int64 listId, const String &address);

      // Whether the list's membership query answers the address now - the
      // accounts it reaches, not its rows.
      static bool QueryAnswers(std::shared_ptr<const DistributionList> list, const String &address);

      // Gives the row a token when it has none, and saves it.
      static bool EnsureToken(std::shared_ptr<DistributionListRecipient> member);

      // ---- joining, by mail and by page --------------------------------------

      enum RequestOutcome
      {
         RequestSent = 0,          // a confirmation went to the address
         RequestRefused = 1,       // the list is closed; the refusal went instead
         RequestAlreadyMember = 2, // nothing sent: a member, or an address the query answers
         RequestThrottled = 3,     // a confirmation went to it within the hour, or a ceiling is reached
         RequestFailed = 4         // not a managed list, not an address, or the row could not be saved
      };

      // requester is who is asking - the page's client address, or for mail
      // the address of the server that handed it to this one - and is what
      // the ceilings on the list's notices are counted against.
      static RequestOutcome RequestSubscription(std::shared_ptr<const DistributionList> list, const String &address, const String &requester);

      // ---- the member's pages ------------------------------------------------

      struct Outcome
      {
         bool ok = false;
         int http_status = 200;
         String heading;
         String detail;
         String list_address;
         String member_address;
         int delivery = 0;
         bool query_member = false;   // an account the query answers: it may leave, and choose nothing else
      };

      // What the confirmation link would do, without doing it.
      static Outcome InspectConfirmation(const String &token);
      static Outcome Confirm(const String &token);

      // The member's own page: what it shows, and its two actions. For an
      // account the membership query answers, queryAddress and queryListId are
      // the address and the list from the link, and the token is the HMAC of
      // the address under the list's secret.
      static Outcome InspectMember(const String &token, const String &queryAddress, __int64 queryListId);
      static Outcome LeaveByToken(const String &token, const String &queryAddress, __int64 queryListId);
      static Outcome SetDeliveryByToken(const String &token, int delivery);

      // The owner's approval page: what the link would do, and doing it for
      // the owner's own signed-in webmail session. signedInAddress is the
      // session's account's address, empty for no session.
      static Outcome InspectApproval(const String &token);
      static Outcome ApproveByToken(const String &token, const String &signedInAddress);

      // ---- administration and self-service ------------------------------------

      // The owner's approval of a request, from the Control Deck or REST.
      static bool Approve(std::shared_ptr<const DistributionList> list, const String &address, String &reason);

      // Clears the member's bounces, and sends each message again to a member
      // the bounces stopped - never to one who chose to pause.
      static bool ResetBounces(std::shared_ptr<const DistributionList> list, const String &address, String &reason);

      // The same, on a row the caller already holds (COM's ResetBounces).
      static bool ResetBounces(std::shared_ptr<DistributionListRecipient> member, String &reason);

      // The member's own choice of delivery, from the webmail. Re-enabling
      // clears the bounces, as the owner's reset does; choosing none makes the
      // pause the member's own, which a reset of the bounces then leaves alone.
      static bool SetDelivery(std::shared_ptr<const DistributionList> list, const String &address, int delivery, String &reason);

      // Removes the member - or, for an address the membership query also
      // answers, keeps a row that says it has left.
      static bool Leave(std::shared_ptr<const DistributionList> list, const String &address, String &reason);

      // ---- copies -----------------------------------------------------------

      // The header lines every copy to this member carries: the RFC 2369 set,
      // RFC 8058's one-click when the member has a way out by link and the base
      // URL is https, Precedence, X-Loop and - for a member with a row - the
      // member tag. queryMember says the member is an account the membership
      // query answers, whose link is made from the list's secret.
      static AnsiString CopyHeaders(std::shared_ptr<const DistributionList> list, const String &member, const String &token, bool queryMember);

      // Queues one copy of contentFile for the member: CopyHeaders, then the
      // file's bytes, signed as the list's domain when that domain has DKIM on
      // and signAsListDomain says so, from the list's bounce address (or the
      // member's VERP form). fromOutside is the posting's Message::GetFromOutside,
      // which the copy carries, so that it is not signed as the domain its From
      // names either; receivedSpfResult is the posting's
      // Message::GetReceivedSpfResult, which the copy carries as well, so that the
      // ARC set sealed over it attests the SPF result this server reached for the
      // posting rather than none, and receivedSpfDomain the domain that check
      // evaluated (Message::GetReceivedSpfDomain), which the set names beside it. The
      // copy is recorded as sent on by the list's domain (Message::GetRelayingDomainID),
      // whose key seals that set. authDomainsOnArrival is what DMARC passed for when the
      // posting arrived (Message::GetAuthDomainsOnArrival), which the copy carries
      // (Message::GetCopiedAuthDomains) for the envelope a copy of it is sent on with in
      // turn - by a member here that forwards it, or an alias a member is. False when it
      // could not be queued; the reason is reported.
      static bool QueueCopy(std::shared_ptr<const DistributionList> list, const String &member, const String &token, const String &contentFile, bool queryMember,
                            bool fromOutside = false, bool signAsListDomain = true, const String &receivedSpfResult = String(),
                            const String &receivedSpfDomain = String(), const String &authDomainsOnArrival = String());

      // Whether the list may sign a copy of this posting as its own domain: always,
      // but for a posting from outside (Message::GetFromOutside) whose From names a
      // domain of the list domain's organisation (the public suffix list's answer, as
      // DMARC's relaxed alignment reads it). The list does not rewrite From, and a
      // signature of its domain would be one DMARC aligns with that From - a pass for
      // a From the list never checked.
      static bool SignsCopiesAsListDomain(bool fromOutside, const String &fromHeaderValue, const String &listAddress);

      // The same for every From field a posting has, as written: not when any of
      // them names an address of the list domain's organisation, whether DMARC here
      // reads it as the author's or not - a second From field, a second address in
      // one, an address in a comment or a group. A receiver may take any of them for
      // the author, and a DKIM signature covers the From field nearest the body: the
      // form above, which reads the first field's first address alone, lets a posting
      // with "From: x@evil.example" above "From: ceo@" the list's domain be signed as
      // the list's domain.
      static bool SignsCopiesAsListDomain(bool fromOutside, const std::vector<String> &fromHeaderValues, const String &listAddress);

      // ---- the pages' budget --------------------------------------------------

      // At most so many unauthenticated requests an hour to the list pages from
      // one address, as the protected-message pages allow; loopback is exempt.
      static bool AllowUnauthenticated(const IPAddress &peer);

      // A request older than this is spent (days).
      static const int RequestLifetimeDays = 7;

   private:
      static bool Distribute_(std::shared_ptr<Message> message, std::shared_ptr<const DistributionList> list, bool &started);
      static void HandleCommand_(std::shared_ptr<Message> message, std::shared_ptr<const DistributionList> list, Command command);
      static void HandleBounce_(std::shared_ptr<Message> message, std::shared_ptr<const DistributionList> list, const String &verpMember);
      static void CountBounce_(std::shared_ptr<const DistributionList> list, std::shared_ptr<DistributionListRecipient> member, bool hard, const String &status);
      static void ConfirmFromMail_(std::shared_ptr<const DistributionList> list, const String &token, const String &sender);
      static bool ConfirmRow_(std::shared_ptr<const DistributionList> list, std::shared_ptr<DistributionListRecipient> member);
      static void RequestLeave_(std::shared_ptr<const DistributionList> list, const String &sender, const String &requester);
      static bool LeaveRow_(std::shared_ptr<const DistributionList> list, std::shared_ptr<DistributionListRecipient> member);
      static bool OptOut_(std::shared_ptr<const DistributionList> list, const String &address);
      static void TellLeft_(std::shared_ptr<const DistributionList> list, const String &address);
      static String MemberLink_(std::shared_ptr<const DistributionList> list, const String &member, const String &token, bool queryMember);
      static bool SendNotice_(std::shared_ptr<const DistributionList> list, const String &to, const String &replyTo,
                              const String &subjectName, const String &textName, const String &subjectSuffix,
                              const std::vector<std::pair<String, String> > &macros);
      static bool IsAutomatic_(const AnsiString &header);
      static bool CarriesLoopFor_(const AnsiString &header, const String &listAddress);
      static String FindToken_(const String &text);
      static bool Throttled_(__int64 listId, const String &address);
      static bool NoticeAllowed_(__int64 listId, const String &listAddress, const String &requester);
      static String RequesterOf_(std::shared_ptr<Message> message);
      static bool IsUsAscii_(const String &value);
      static __int64 Now_();
   };

   // Run from ClassTester::DoTests: the address grammar, VERP both ways, the
   // token text rule, a member tag's shape and the query member's token.
   class ListManagerTester
   {
   public:
      void Test();
   };
}
