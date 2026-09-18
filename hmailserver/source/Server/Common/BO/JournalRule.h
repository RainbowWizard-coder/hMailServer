// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// One journaling rule: a copy of every message of a domain, or of one account,
// to an address - Exchange's journaling and Google's dual delivery, which are
// the same mechanism with two shapes of copy.
//
// A rule names WHOSE mail (a domain, or one account in it), WHICH WAY (mail
// arriving for them, mail they send when authenticated, or both), WHERE the
// copy goes (one address, local or elsewhere) and WHAT SHAPE it takes:
//
//   * report - an envelope report in the Exchange journal shape: Sender,
//     Subject, Message-Id and one Recipient line per envelope recipient in a
//     text part, and the original message attached as message/rfc822, byte
//     for byte as it was accepted. A compliance archive wants this, because
//     the envelope is what the message's own headers cannot be trusted to say.
//   * copy - the original message itself, sent as it is to the address. A
//     migration or a second system wants this, because it wants the mail, not
//     a report about it.
//
// The rule is evaluated once per accepted message, after its recipients are
// known, in Journaling (Common/Util/Journaling.h); one report or copy per
// matching rule per message, queued through the ordinary delivery queue. What
// it deliberately is not: not the on-disk archive (ArchiveDir keeps files on
// this server; this sends mail elsewhere), not an account's forwarding (which
// the account holder controls and which delivers instead of, or as well as,
// the mailbox), not a rule's forward action (which runs per delivery attempt
// and can be edited by the account), and not retention - the copy is the
// journal system's from the moment it leaves the queue.
//
// The domain and the account are held by id, as every other per-domain record
// is, and the target the administrator typed - a domain name or an account
// address - is resolved to those ids when the rule is saved and back to a name
// when it is read, so a rule survives nothing being renamed under it in the
// file and a backup carries the name rather than a number the restore reissues.

#pragma once

namespace HM
{
   class JournalRule : public BusinessObject<JournalRule>
   {
   public:

      // Whose mail the rule is about. Persisted in hm_journalrules.jrscope and
      // given by name over REST and COM, so never renumbered.
      enum Scope
      {
         ScopeDomain = 0,
         ScopeAccount = 1
      };

      // Which way the mail is going, seen from the domain or account named.
      // Inbound is a message with a local recipient the rule covers; outbound is
      // a message from an authenticated sender the rule covers.
      enum Direction
      {
         DirectionInbound = 0,
         DirectionOutbound = 1,
         DirectionBoth = 2
      };

      // The shape of the copy: the envelope report with the original attached,
      // or the original as it is.
      enum Mode
      {
         ModeReport = 0,
         ModeCopy = 1
      };

      JournalRule();
      virtual ~JournalRule();

      // The collection's name lookup and the restore's error line. The target as
      // typed, which is what an administrator recognises.
      String GetName() const { return target_; }

      void SetScope(Scope value) { scope_ = value; }
      Scope GetScope() const { return scope_; }

      // The domain the rule belongs to: the domain named, or the domain of the
      // account named. Every rule has one, which is what lets the rules go with
      // the domain when it is deleted and be carried under it in a backup.
      void SetDomainID(__int64 value) { domain_id_ = value; }
      __int64 GetDomainID() const { return domain_id_; }

      // The account named, for an account rule; 0 for a domain rule.
      void SetAccountID(__int64 value) { account_id_ = value; }
      __int64 GetAccountID() const { return account_id_; }

      // The domain name or the account address, as typed and as shown. Resolved
      // to the two ids by PersistentJournalRule when the rule is saved, and from
      // them when it is read; not a column of its own.
      void SetTarget(const String &value) { target_ = value; }
      String GetTarget() const { return target_; }

      // Where the copy goes. One address, local or elsewhere.
      void SetJournalAddress(const String &value) { journal_address_ = value; }
      String GetJournalAddress() const { return journal_address_; }

      void SetDirection(Direction value) { direction_ = value; }
      Direction GetDirection() const { return direction_; }

      void SetMode(Mode value) { mode_ = value; }
      Mode GetMode() const { return mode_; }

      void SetEnabled(bool value) { enabled_ = value; }
      bool GetEnabled() const { return enabled_; }

      void SetNote(const String &value) { note_ = value; }
      String GetNote() const { return note_; }

      // Whether the rule speaks for mail going this way.
      bool CoversInbound() const { return direction_ == DirectionInbound || direction_ == DirectionBoth; }
      bool CoversOutbound() const { return direction_ == DirectionOutbound || direction_ == DirectionBoth; }

      bool XMLStore(XNode *parentNode, int options);
      bool XMLLoad(XNode *node, int options);
      bool XMLLoadSubItems(XNode *node, int options) { return true; }

   protected:

      Scope scope_;
      __int64 domain_id_;
      __int64 account_id_;
      String target_;
      String journal_address_;
      Direction direction_;
      Mode mode_;
      bool enabled_;
      String note_;
   };
}
