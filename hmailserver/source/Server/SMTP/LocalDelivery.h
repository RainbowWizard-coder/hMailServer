// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include "../Common/Sieve/SieveEvaluator.h"
#include "../Common/Persistence/MessageStore.h"
#include "../Common/Util/MailboxKeys.h"
#include "../Common/AntiSpam/SenderListCache.h"
#include "DeliveryFailure.h"

namespace HM
{
   class Message;
   class MessageRecipient;
   class MimeHeader;
   class RuleResult;

   class LocalDelivery
   {
   public:
      LocalDelivery(const String &sSendersIP, std::shared_ptr<Message> message, const RuleResult &globalRuleResult);
      ~LocalDelivery(void);

      bool Perform(std::vector<DeliveryFailure> &saErrorMessages);

      // The local recipients Perform held rather than delivered: whether the
      // mailbox is encrypted at rest was not known (MailboxKeys::KeyedUnknown -
      // the accounts' keys not read since the start, or a key row that holds
      // no key). Their rows are left in the database, and they are taken off
      // the message once the loop is over, so that no later recipient took the
      // message's file for its own copy meanwhile.
      const std::vector<std::shared_ptr<MessageRecipient> > &GetHeldForKeys() const { return held_for_keys_; }

      // The held recipients: when the message is already rescheduled (an
      // external recipient's deferral) they go with it; otherwise the message
      // is rescheduled here, on the SMTP retry schedule, while it has retries
      // left - as ListManager::HoldForRetry does for a posting it could not
      // read - and when it has none each is reported to the sender as a class
      // 4 failure (4.3.0, this server's) and its row removed. True when the
      // message stays in the queue. Never a bounce while retries are left,
      // never a message discarded, never one stored in the clear.
      static bool HoldForRetry(std::shared_ptr<Message> message, const std::vector<std::shared_ptr<MessageRecipient> > &held,
                               bool alreadyRescheduled, std::vector<DeliveryFailure> &failures);

      // What the sender of a message to a mailbox whose key row holds no key
      // is told: a sender this server hosts - the organisation's own - the
      // sentence with the way out (MailboxKeys::DamagedSentence); anybody else
      // only that the mailbox cannot take mail now, and nothing of how it is
      // stored or how an administrator mends it. The error log has the whole.
      static String DamagedReportFor(const String &sender);

      // The addheader and deleteheader edits a Sieve script decided (RFC 5293), made
      // to a message's header in script order: what ApplySieveHeaderEdits_ does to
      // the stored file, without the file. An edit of a name - as the script's
      // variables expanded it - that SieveParser::WhyFieldMayNotBeEdited refuses is
      // not made, and its command and the refusal go into refused; a value added is
      // kept to one field (RuleGuard::HeaderValueAsOneField). False when every edit
      // was refused, so that a header nothing was done to is not written back.
      static bool EditSieveHeader(MimeHeader &header, const std::vector<SieveHeaderEdit> &edits,
                                  std::vector<String> &refused);

   private:
      
      void DeliverToLocalAccount_(std::shared_ptr<const Account> account, size_t iNoOfRecipients, std::vector<DeliveryFailure> &saErrorMessages, const String &sOriginalAddress, bool &messageReused, bool suppressFailureDsn);
      // suppressFailureDsn is the recipient's RFC 3461 NOTIFY opt-out, threaded
      // through because a Sieve reject sends its non-delivery report from here.
      bool LocalDeliveryPreProcess_(std::shared_ptr<const Account> account, std::shared_ptr<Message> accountLevelMessage, const String &sOriginalAddress, std::vector<DeliveryFailure> &saErrorMessages, bool suppressFailureDsn);
      bool AddTraceHeaders_(std::shared_ptr<const Account> account, std::shared_ptr<Message> pMessage, const String &sOriginalAddress);

      // Applies the account's own spam settings to its copy of the message, before
      // anything else looks at that copy. Everything here re-judges a verdict the
      // conversation already reached - it never re-runs a test - because the spam
      // flag and the recorded X-hMailServer-Reason-Score header are all that
      // survive from the conversation to delivery. A message the global filter did
      // not classify carries neither, so for it this method does nothing, which is
      // exactly the pre-feature behaviour.
      //
      // Returns false when this account's copy must not be delivered: its
      // per-account delete threshold was provably reached (the recorded score when
      // present, otherwise the global mark threshold as a lower bound - never a
      // guess). The copy is first placed in the quarantine store when that is
      // enabled; a quarantine failure delivers the marked copy instead, because
      // discarding mail that was not actually stored would be silent loss.
      //
      // May instead unmark the copy in place (opt-out, or a mark-threshold
      // override the recorded score falls short of): flag cleared, spam headers
      // and subject tag rewritten out of this account's file.
      bool ApplyAccountSpamOverrides_(std::shared_ptr<const Account> account, std::shared_ptr<Message> accountLevelMessage);

      // The reader's own allow and block lists, and the domain's and the
      // server's, applied to this copy before the thresholds above. The
      // precedence between the three scopes lives in SenderListCache::Decide;
      // this is what the decision does to a message.
      //
      // Returns false when the copy must not be delivered - a blocked sender
      // whose message the quarantine accepted, so that there is something for
      // the recipient to release from their held-mail page and their digest.
      //
      // `decided` is true when a list spoke for this copy and the per-account
      // thresholds must therefore not run: an allowed sender has skipped the
      // spam verdict, and a blocked one has already been marked. It is
      // deliberately separate from the return value, which is only about
      // whether the copy is delivered at all.
      bool ApplySenderLists_(std::shared_ptr<const Account> account, std::shared_ptr<Message> accountLevelMessage, bool &decided);

      // Marks this account's copy as spam because a block list named its sender:
      // the flag, the X-hMailServer-Spam and X-hMailServer-Reason-1 headers and
      // the configured subject tag, so that the copy reaches whatever rule or
      // Sieve script the account already uses for spam. No Reason-Score header,
      // because nothing measured this message.
      static void MarkAsBlocked_(std::shared_ptr<const Account> account, std::shared_ptr<Message> message,
                                 const String &senderAddress, const String &scopeName, const String &entry);

      // The address in the message's From header, read once for the whole
      // delivery and empty when there is none. The lists match it as well as the
      // envelope sender; see SenderListCache.
      const String &HeaderFromAddress_(std::shared_ptr<const Account> account, std::shared_ptr<Message> accountLevelMessage);
      String header_from_address_;
      bool header_from_read_ = false;

      // The message's links, read for the URL entries of the sender lists once
      // for all its recipients (SenderListCache::ReadUrlLinks), when the first
      // recipient with such an entry is reached; read again only when the list
      // was read again from the database meanwhile.
      SenderListCache::UrlLinks url_links_;
      bool url_links_read_ = false;

      // The total score the conversation recorded on the message - the
      // X-hMailServer-Reason-Score header, written when "add reason headers" is on
      // and the message was classified. Returns -1 when absent or unparseable;
      // never guesses.
      static int ReadRecordedSpamScore_(std::shared_ptr<const Account> account, std::shared_ptr<Message> message);

      // Removes the classification from this account's copy: the spam flag on the
      // message object, the X-hMailServer-Spam / X-hMailServer-Reason-* headers,
      // and the prepended subject tag, so the copy reads as if it was never
      // classified. Only this copy - the file is already the account's own.
      static void RemoveSpamClassification_(std::shared_ptr<const Account> account, std::shared_ptr<Message> message);

      // The account's own vacation message. Returns true when that feature is ON
      // for this account - whether or not a reply was actually produced (the
      // sender may be suppressed, the message spam-flagged, the sender the account
      // itself). The return value is what keeps the precedence rule honest: an
      // account that has spoken for itself, even by staying silent, is never
      // spoken over by the domain-wide reply, so exactly one auto-reply can ever
      // answer one delivered message.
      //
      // messageIsSpamForThisAccount is the flag on the ACCOUNT'S copy, after the
      // per-account overrides ran - an account that opted out of spam filtering
      // has its auto-reply suppression judged against its own view of the message,
      // not against the shared original's flag.
      bool SendAutoReplyMessage_(std::shared_ptr<const Account> pAccount, std::shared_ptr<Message> pMessage, bool messageIsSpamForThisAccount);

      // The domain-wide out-of-office reply, for an account with no active
      // vacation message of its own. Chooses the domain's internal or external
      // text by what the envelope sender resolves to (see
      // SMTPVacationMessageCreator::IsInternalSender) and sends through the same
      // creator the account-level reply uses, so the RFC 3834 loop guards and the
      // once-per-sender memory exist exactly once. Called after the Sieve script
      // has been evaluated, and only when neither the account's stored vacation
      // message nor a Sieve vacation action was active - the account's own voice,
      // in either form, wins over the domain's.
      void SendDomainAutoReplyMessage_(std::shared_ptr<const Account> pAccount, std::shared_ptr<Message> pMessage, bool messageIsSpamForThisAccount);
      bool RunAccountRules_(std::shared_ptr<const Account> pAccount, std::shared_ptr<Message> pMessage, RuleResult &accountRuleResult);

      // The copy an account's own filter is about to drop - a rule's Delete or
      // Reject, a Sieve discard, reject or redirect that keeps nothing, a
      // forward that keeps no copy, the account's spam delete threshold - kept
      // among the account's deleted items instead when a legal hold covers the
      // account (docs/Discovery.md), and the log says so. For a zero-access
      // mailbox the kept copy is sealed to its key first, as the mailbox would
      // have held it; the copy is never kept in the clear. The copy is still not
      // delivered to the mailbox: what the filter decided stands, and what it
      // would have destroyed is what the hold keeps.
      //
      // True when the drop may go ahead: no hold covers the account, or the
      // copy was kept. False when a hold covers it and the copy could not be
      // kept: the caller then delivers the message instead of dropping it,
      // since a hold outranks the filter, and the log says that too.
      bool KeepHeldCopy_(std::shared_ptr<const Account> account, std::shared_ptr<Message> message, const String &dropped);

      bool CheckAccountQuotas_(std::shared_ptr<const Account> pAccount, std::vector<DeliveryFailure> &saErrorMessages, bool suppressFailureDsn);

      // Evaluates the recipient account's active Sieve script (if any) against the
      // message, firing any redirect actions and any RFC 5230 vacation auto-reply.
      // Sets sieveFolder to a fileinto target (empty when none) and sieveDrop true
      // when the local copy should not be delivered (a discard, or a redirect that
      // cancels the implicit keep).
      //
      // sOriginalAddress is the address this delivery was addressed to, which is not
      // always the account's own address (an alias or a plus-address reaches it too).
      // It is the SMTP envelope recipient the evaluator needs for "envelope" tests
      // and for the RFC 5230 4.5 check that refuses to auto-reply to a message the
      // user was not actually a recipient of.
      // sieveFlagsGiven is true when the script set the local copy's IMAP flags -
      // through :flags on the keep/fileinto, or through setflag/addflag/removeflag -
      // and sieveFlags is then the final set it decided on, canonicalised by
      // SieveParser::SplitFlagList. Empty-and-given is meaningful and different from
      // not-given: "removeflag" that cleared everything is an instruction, and a
      // script that never mentioned flags is not.
      // sieveRejectReason is non-empty when the script refused the message with
      // reject/ereject (RFC 5429): the caller then drops the local copy AND sends
      // a non-delivery report carrying the reason, through the same machinery as
      // a quota bounce.
      // sieveVacationRequested is true when the script asked for a vacation reply
      // to this message - whether or not the responder's own suppression window
      // then let it out. It exists for the domain-wide out-of-office reply's
      // precedence decision, which is about who SPEAKS for the account, not about
      // whether this particular reply escaped the rate limit.
      void EvaluateSieveScript_(std::shared_ptr<const Account> account, std::shared_ptr<Message> message,
                                const String &sOriginalAddress, String &sieveFolder, bool &sieveDrop,
                                bool &sieveFlagsGiven, std::vector<String> &sieveFlags,
                                String &sieveRejectReason, bool &sieveVacationRequested);

      // Writes the flags a script decided onto the message that is about to be saved.
      //
      // Separate from the evaluation for the same reason sieveFolder and sieveDrop are
      // out-parameters rather than actions: the evaluator decides and the delivery
      // acts, so there is one place to look for what a script actually did to a
      // message.
      static void ApplySieveFlags_(std::shared_ptr<const Account> account, std::shared_ptr<Message> message,
                                   const std::vector<String> &sieveFlags);

      // Rewrites the stored message file with the addheader/deleteheader edits a
      // script decided (RFC 5293), in script order, and refreshes the message's
      // recorded size. Runs BEFORE redirects are queued, so a redirected copy
      // carries the edits - RFC 5293 7: an edit affects all subsequent actions.
      // A failed write is reported through RuleGuard::ReportActionFailed, the
      // same visibility the rules engine's header rewriting gets, and an edit
      // EditSieveHeader refuses with a line in the application log.
      void ApplySieveHeaderEdits_(std::shared_ptr<const Account> account, std::shared_ptr<Message> message,
                                  const std::vector<SieveHeaderEdit> &edits);

      // Exact, case-sensitive membership. The flag names reaching here have already
      // been canonicalised by SieveParser::SplitFlagList, so a case-insensitive test
      // would compare values that cannot differ in case and would quietly go on
      // working if that canonicalisation were ever removed.
      static bool ListContains_(const std::vector<String> &values, const String &value);

      std::shared_ptr<Message>  CreateAccountLevelMessage_(std::shared_ptr<Message> pOriginalMessage, std::shared_ptr<const Account> pRecipientAccount, bool reuseMessage, const String &sOriginalAddress, bool &tracedAlready);

      // DeliveryHardLinks. Every local copy gets its trace headers by a rewrite, and
      // a rewrite of a name is a new file, so a link from the queue file never
      // survives delivery. The template is one finished copy - a second name for
      // the queue file with the trace headers written to it - from which every
      // sharing recipient's file is linked; made on first use, removed when the
      // delivery is over, its content living on under the recipients' names.
      bool SharingPossible_(std::shared_ptr<const Account> account) const;
      String SharedTemplate_(std::shared_ptr<const Account> account);
      String shared_template_;
      bool shared_template_failed_ = false;
      bool current_copy_traced_ = false;

      // The copy becomes a stored message (Persistence/MessageStore.h): compressed
      // and its large parts held once, when MessageStoreCompression is on. The
      // first copy that is still a name of the template above is placed where it
      // is, and each later one is made a name of that placed file, so the
      // recipients go on sharing one file. False when the store refuses the copy
      // (MessageStore::PlaceAs), which is then not to be kept, with what the
      // refusal was. The mailbox's encryption is asked again at placing, and
      // what Perform read before the copy was made stands in when that cannot
      // say - a recipient Perform could not read it for was held - so a copy
      // is never refused for not knowing.
      bool PlaceCopy_(std::shared_ptr<const Account> account, std::shared_ptr<Message> message, MessageStore::PlaceRefusal &refusal);
      MailboxKeys::Encryption encryption_before_ = MailboxKeys::EncryptionNone;
      std::string key_before_;
      std::vector<std::shared_ptr<MessageRecipient> > held_for_keys_;
      String placed_template_;
      bool placed_template_failed_ = false;

      const String &_sendersIP;
      const std::shared_ptr<Message> original_message_;
      const RuleResult &_globalRuleResult;

   };
}