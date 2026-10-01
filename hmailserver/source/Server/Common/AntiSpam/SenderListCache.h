// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <map>
#include <set>
#include <vector>

namespace HM
{
   class MessageData;
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

      // What the links of one message are to the URL entries, read once for
      // all its recipients (ReadUrlLinks): for each URL entry's host that
      // covers a link of the message, the first such link's host, in the
      // order the links come. Read against the list as it was at
      // `generation`; a recipient decided after the list was read again from
      // the database has the links read again (IsCurrent).
      struct UrlLinks
      {
         unsigned long long generation = 0;
         std::map<String, String> covered;
      };

      // Reads every link of the message (SURBL::ForEachLinkHost - every one,
      // however many: a cap on the links read would let a message put its
      // blocked link after that many others) against the hosts of every
      // unexpired URL entry, at every scope. Memory is one map entry for each
      // entry host a link is under, whatever the message holds, and the time
      // is one reading of the text and, for each link, one lookup for each of
      // its labels.
      static void ReadUrlLinks(std::shared_ptr<MessageData> messageData, UrlLinks &links);

      // One link's host matched against the entry hosts: each entry host
      // that covers it (SenderPattern::UrlEntryHostsCovering) is recorded with
      // it, unless an earlier link was. ReadUrlLinks calls it for every link.
      static void CoverLink(const std::set<String> &entryHosts, const String &linkHost, std::map<String, String> &covered);

      // Whether links were read against the list as it is now.
      static bool IsCurrent(const UrlLinks &links);

      // The same decision for the links in a message: a URL entry
      // (SenderPattern::IsUrl) at the nearest scope with an opinion about any
      // of them decides, the more specific entry winning within a scope and a
      // block winning a tie, as for senders. `matched` names the link's host.
      // Nothing is decided by an entry that is not a URL, and nothing about a
      // message with no links.
      static Decision DecideUrls(const UrlLinks &links, __int64 accountId, __int64 domainId);

      // Whether any unexpired URL entry exists at the three scopes this
      // recipient is under, so that the links are extracted from a message
      // only when somebody has listed one.
      static bool HasUrlEntries(__int64 accountId, __int64 domainId);

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
      static Decision EvaluateUrls_(const UrlLinks &links, int scope, __int64 ownerId);
      static bool OwnerAtScope_(int scope, __int64 accountId, __int64 domainId, __int64 &ownerId);

      static std::vector<std::shared_ptr<SenderListEntry> > entries_;
      static bool need_refresh_;

      // One more each time the entries are read from the database.
      static unsigned long long generation_;
   };
}
