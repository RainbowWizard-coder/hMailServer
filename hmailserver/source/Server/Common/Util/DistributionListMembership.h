// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Distribution lists that define themselves: a list whose members are a query
// over the server's accounts, evaluated when a message is sent to the list
// rather than maintained by hand. Exchange calls the shape a dynamic
// distribution group.
//
// The query is one short text on the list, hm_distributionlists.
// distributionlistmembership (schema 6074), in one of five forms:
//
//    (empty)                  a static list: the rows in
//                             hm_distributionlistsrecipients and nothing else,
//                             which is every list made before this existed
//    domain                   every active account of the list's own domain
//    domain:<name>            every active account of the named domain, which
//                             this server has to host
//    pattern:<wildcard>       every active account whose address matches the
//                             wildcard, * for any run of characters and ? for
//                             one, case ignored; sales-*@example.com,
//                             *.london@example.com
//    everyone                 every active account of every active domain on
//                             the server
//
// The static rows stay: a query list's members are the union of its rows and
// what its query answers, so the one contractor outside the domain is added by
// hand beside "everyone in the domain". RecipientParser evaluates the query at
// RCPT TO through Members, each address then going through the same expansion
// a static member does, and AddRecipient_ deduplicates by address as it always
// has - an account that is both a row and a query answer is delivered to once.
//
// A directory attribute is not a form, and the reason is a choice rather than
// an absence. hm_accounts carries no department and no title - the first and
// last name are the whole of what it says about the person - but the server
// does group accounts: hm_groups and hm_group_members, administered over COM,
// the Control Panel, the Deck and REST. group:<name> would be one more branch
// here and one more select joining those two tables to hm_accounts, and it is
// deliberately not built rather than unavailable: a group is an ACL principal
// for shared folders, not a statement about the person, and it is server-wide
// and belongs to no domain, so like everyone it would be a form only the
// server administrator could set. If a group is ever to be a mail concept as
// well, this parser is where that form goes. DistributionLists.md says the
// same in as many words.
//
// The query is judged at save time by Validate (PreSaveLimitationsCheck, so
// COM and REST refuse the same text in the same sentence), which is where a
// domain the server does not host or a pattern with no @ is caught; a query
// that was valid and stopped being so - the named domain deleted since - is
// not an error at delivery, it answers nobody, and the members route says
// why.
//
// "Answers nobody" and "could not be answered" are two different things and
// are never returned as the same one. A query answering nobody is a list that
// has grown smaller: the message goes to the list's rows. A query that could
// not be answered - the accounts could not be read, the domain it names could
// not be read - is not a membership at all, and every caller is told so
// separately (the unavailable flag on Resolve and Members) because the union
// with the rows would otherwise hide it perfectly: one row expands, the RCPT
// is accepted with 250, and a message for a thousand people reaches one of
// them with nothing logged and nothing retried.
#pragma once

namespace HM
{
   class DistributionList;

   class DistributionListMembership
   {
   public:
      enum Kind
      {
         KindStatic = 0,
         KindOwnDomain = 1,
         KindNamedDomain = 2,
         KindPattern = 3,
         KindEveryone = 4
      };

      struct Query
      {
         Query() : kind(KindStatic) { }

         Kind kind;

         // The domain name for KindNamedDomain, the wildcard for KindPattern,
         // lower-cased; empty otherwise.
         String argument;

         // The one domain the query can answer accounts of, lower-cased, or
         // empty when it can answer accounts of any domain (everyone, and a
         // pattern whose domain part has a wildcard in it). Own domain answers
         // the list's own, which the caller knows and this cannot.
         String scope_domain;
      };

      // The longest text the column holds.
      static constexpr int MaxLength = 200;

      // Reads the text. False with reason when it is none of the five forms;
      // the reason is the sentence the administrator sees.
      static bool Parse(const String &text, Query &query, String &reason);

      // Parse, then the checks that need the server: a named domain has to be
      // one this server hosts. normalised is the text as it will be stored -
      // trimmed, lower-cased, the form words spelled as documented.
      static bool Validate(const String &text, String &normalised, String &reason);

      // Whether a credential that administers only the named domains may set
      // this query on a list of ownDomain: a query that reaches beyond them is
      // the server administrator's. callerDomains empty means unconfined.
      static bool WithinDomains(const Query &query, const String &ownDomain, const std::vector<String> &callerDomains);

      // The addresses the query answers now: active accounts in active domains,
      // lower-cased, sorted, unique. Empty for a static list. problem is set,
      // with the addresses empty, when the query names a domain this server no
      // longer hosts.
      //
      // unavailable is the difference that decides what a caller may do with an
      // empty answer: false means the query WAS answered, even if with nobody,
      // and true means it could not be - the accounts, or the domain the query
      // names, could not be read. An empty answer of the second kind is not a
      // membership and must not be delivered on, because a query list with rows
      // would otherwise take a message for its rows alone and answer 250 for
      // everybody. Both callers pass it; neither may ignore it.
      static std::vector<String> Resolve(const Query &query, __int64 ownDomainId, String &problem, bool &unavailable);
      static std::vector<String> Resolve(const String &text, __int64 ownDomainId, String &problem, bool &unavailable);

      // The list's members as a message to it is expanded: its static rows in
      // their order, then what its query answers that the rows did not name.
      // unavailable as above - true means the answer is not the membership and
      // the caller must refuse rather than deliver to what came back.
      static std::vector<String> Members(std::shared_ptr<const DistributionList> list, bool &unavailable);

      // Whether an address is a member: a static row or a query answer, the
      // domain aliases already applied to the address by the caller.
      static bool IsMember(std::shared_ptr<const DistributionList> list, const String &address);

      // A domain was renamed: every list whose query names the old name by its
      // domain form or by the domain part of its pattern is rewritten to the
      // new one. False with the reason when a list could not be saved.
      static bool RenameDomain(const String &oldName, const String &newName, String &reason);

      // The word for a kind, for the members route and the log.
      static const char *KindWord(Kind kind);

   private:

      // failed says the read did not happen, which is never the same answer as
      // "no account matched"; each of these sets it and never clears it, so one
      // flag can carry a whole resolution.
      static std::vector<String> AccountsOfDomain(__int64 domainId, bool &failed);
      static std::vector<String> AccountsMatching(const String &pattern, const String &scopeDomain, bool &failed);
      static std::vector<String> AccountsOfEveryDomain(bool &failed);
   };
}
