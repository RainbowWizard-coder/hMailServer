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
   // WHICH ADDRESS IS "THE SENDER"
   //
   // Both of them: the envelope sender (MAIL FROM) and the address in the From
   // header. An entry covers a message when it matches either. The From header is
   // the address a person sees and the one the webmail's "Block this sender"
   // writes, and mail sent through a mailing service carries the service's own
   // address in the envelope - an entry that looked only at the envelope would
   // never match the newsletter the reader was trying to block or to rescue.
   //
   // Neither address is proof of anything. Both are whatever the sender wrote, so
   // an allow entry is exactly as forgeable as the address it names: somebody who
   // knows a reader trusts example.com can write example.com. That is true of
   // every safe-sender list there is, and it is why an allow entry is best kept to
   // a correspondent whose domain publishes DMARC - and why the documentation says
   // so rather than this list pretending otherwise.
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
   // WITHIN one scope, when two entries both match - through either address:
   //
   //   * the more specific entry wins - an address beats a domain, and a longer
   //     domain beats a shorter one - so "mail.example.com" is an exception to
   //     "example.com" rather than an argument with it;
   //   * on an exact tie, BLOCK wins. A list that says both things about one
   //     message has written a contradiction, and the safe reading of a
   //     contradiction about mail is the one that does not deliver it into the
   //     inbox. (The REST and COM writers replace rather than duplicate one
   //     value, so a tie is an allow on one address and a block on the other of
   //     equal specificity, or a row pair that got in some other way.)
   //
   // An expired entry is not consulted at all, whether or not the sweep has got
   // to it yet. And a message with the null sender - a bounce, a delivery status
   // notification - is never decided by any list, whatever its From header says:
   // a list that could swallow a DSN would make a delivery failure invisible to
   // the person who sent the message.
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

         // Which scope decided, the entry's own text and reason, and which of the
         // two addresses it matched - for the log line and the reason header, so
         // that "why did this arrive unmarked" has an answer in the message itself.
         int scope = 0;
         String value;
         String reason;
         String matched;
      };

      // The decision for one message delivered to one account. envelopeSender is
      // MAIL FROM; headerFrom is the address in the From header, empty when there
      // is none or it could not be read. domainId is the account's domain; pass
      // 0 for a recipient that has none (nothing can then match at domain scope).
      static Decision Decide(const String &envelopeSender, const String &headerFrom, __int64 accountId, __int64 domainId);

      static void SetNeedRefresh();

      // True when no entry exists at any scope, which lets the delivery path
      // skip the whole feature on an installation where nobody uses it - the
      // common case, and the one that must cost nothing. Checked before the
      // message's From header is read.
      static bool IsEmpty();

      // The scope's name, for a log line and for the header. Not localised: it
      // goes in the server's log and in a message header, both of which are read
      // in English by people comparing them with this source.
      static String ScopeName(int scope);

   private:

      static void Refresh_();
      static Decision Evaluate_(const String &envelopeSender, const String &headerFrom, int scope, __int64 ownerId);

      static std::vector<std::shared_ptr<SenderListEntry> > entries_;
      static bool need_refresh_;
   };
}
