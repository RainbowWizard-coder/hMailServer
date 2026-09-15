// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <vector>

namespace HM
{
   class SenderListEntry;

   // The sender list, held in memory, and the one place the precedence between
   // the three scopes is decided.
   //
   // THE PRECEDENCE, IN FULL
   //
   // Nearest first: the account, then its domain, then the server. The first
   // scope that has anything to say about this sender is the ONLY scope that is
   // consulted - a domain block does not survive the account holder allowing
   // that sender, and a server block does not survive either of them.
   //
   // That is a deliberate choice and it is not the one Microsoft 365 makes; its
   // tenant block list beats a user's safe senders. The argument for this way
   // round is that the message is addressed to the account holder, the account
   // holder is the person who can tell whether they wanted it, and an
   // administrator who needs a sender stopped for everyone regardless has a
   // mechanism that already does exactly that and cannot be overridden: the
   // server-wide blocked senders list, which refuses the message during the
   // SMTP conversation before any recipient's preferences are reachable. Two
   // mechanisms, two strengths, and the difference is written down rather than
   // discovered.
   //
   // WITHIN one scope, when two entries both match:
   //
   //   * the more specific entry wins - an address beats a domain, and a longer
   //     domain beats a shorter one - so "mail.example.com" is an exception to
   //     "example.com" rather than an argument with it;
   //   * on an exact tie, BLOCK wins. A person who has written the same address
   //     into both halves of their own list has written a contradiction, and the
   //     safe reading of a contradiction about mail is the one that does not
   //     deliver it. (The REST and COM writers replace rather than duplicate, so
   //     this is the answer to a row pair that got in some other way - a
   //     restore, a hand-written SQL statement - not to anything a person can do
   //     through an editor.)
   //
   // An expired entry is not consulted at all, whether or not the sweep has got
   // to it yet.
   class SenderListCache
   {
   public:
      enum Verdict
      {
         // Nobody has listed this sender at any scope. The spam verdict stands.
         NoOpinion = 0,

         // Somebody nearer than the server said this sender is wanted.
         Allowed = 1,

         // Somebody said this sender is not wanted by this recipient.
         Blocked = 2
      };

      struct Decision
      {
         Verdict verdict = NoOpinion;

         // Which scope decided, and the entry's own text - for the log line and
         // for the X-hMailServer-SenderList header, so that "why did this arrive
         // unmarked" has an answer in the message itself.
         int scope = 0;
         String value;
         String reason;
      };

      // The decision for one envelope sender delivered to one account.
      // domainId is the account's domain; pass 0 for a recipient that has none
      // (nothing can then match at domain scope).
      static Decision Decide(const String &senderAddress, __int64 accountId, __int64 domainId);

      // The decision at one scope only, which is what the editors use to show a
      // person what their own list says without the other two scopes confusing
      // the answer.
      static Decision DecideAtScope(const String &senderAddress, int scope, __int64 ownerId);

      static void SetNeedRefresh();

      // True when no entry exists at any scope, which lets the delivery path
      // skip the whole feature on an installation where nobody uses it - the
      // common case, and the one that must cost nothing.
      static bool IsEmpty();

      // The scope's name, for a log line and for the header. Not localised: it
      // goes in the server's log and in a message header, both of which are read
      // in English by people comparing them with this source.
      static String ScopeName(int scope);

   private:

      static void Refresh_();
      static Decision Evaluate_(const String &senderAddress, int scope, __int64 ownerId);

      static std::vector<std::shared_ptr<SenderListEntry> > entries_;
      static bool need_refresh_;
   };
}
