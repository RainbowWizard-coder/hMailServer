// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The membership query of a distribution list, over REST. See
// Common/Util/DistributionListMembership.h for what a query is and
// RestApiAdministration.cpp for the list routes it is a field of.
//
//    /api/v1/lists/{address}/members    what the list delivers to now
//
// The field itself - "membership" - rides on the list document that the
// listing, the create and the update already carry, because it is one more
// property of a list rather than a resource of its own. What needed a route is
// the answer to the question the field raises: the list says "domain", so who
// is that this morning? The route resolves the query against the accounts that
// exist at the moment it is called and answers the addresses, each marked with
// where it came from - a row of hm_distributionlistsrecipients, or the query -
// and says plainly when the query answers nobody and why.
//
// It is a GET and it reads accounts of the list's own domain, so it sits with
// the listing under ViewAccounts and is scoped by DomainNamedByRoute_ to the
// domain in the list's address, exactly as the update and the delete are.
//
// That scoping is not the whole of the authorisation, because the query on the
// list need not be about the list's own domain: the server administrator may
// set everyone, or domain:another-tenant.example, on a list in a domain a
// confined credential administers - the write side refuses a confined
// credential exactly those forms, and would otherwise be answering this route's
// caller with every address of the other tenant. So the same question the
// writes ask, WithinDomains, is asked here before anything is resolved, and a
// query that reaches past the caller's domains is 403 rather than a list of
// other people's mailboxes. When the answer IS given, every address in it is in
// a domain the caller holds, by the same rule.
//
// What it deliberately does not do: it does not store the answer anywhere, and
// nothing caches it. A query list has no membership table to drift out of date,
// which is the whole point of the row; the cost is that this route, and the
// expansion at RCPT TO, each ask the database at the moment they are asked.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "Unicode.h"

#include "../BO/Domain.h"
#include "../BO/DistributionList.h"
#include "../BO/DistributionLists.h"
#include "../BO/DistributionListRecipients.h"
#include "../BO/DistributionListRecipient.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentDistributionListRecipient.h"
#include "../Util/DistributionListMembership.h"
#include "../Util/Parsing/StringParser.h"

#include <set>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   AnsiString
   RestApiServer::OpenApiDistributionListMembersPath_()
   {
      // Begins with a comma and does not end with one, as every piece
      // HandleOpenApi_ appends does.
      return AnsiString(
         ",\"/api/v1/lists/{address}/members\":{"
         "\"get\":{\"summary\":\"Who a distribution list delivers to now\",\"description\":\"The list's own rows, then the accounts its membership query answers at the moment of the call, deduplicated by address. Each entry: address and source - row for a member kept by hand, query for one the membership found. Also membership (the query as stored), membership_kind (static, domain, named_domain, pattern, everyone, or unreadable for a text this build cannot parse), count, and problem when the query could not be answered - a domain it names that this server no longer hosts, or a disabled one - in which case only the rows are listed, which is exactly what a message to the list would reach. Nothing is stored: a query list has no membership table, and this is the query being run. Scoped to the domain in the address, and refused with 403 to a credential confined to domains the query reaches past - the forms that credential may not set are the forms it may not read the answer to. 503 when the accounts could not be read, rather than a short answer that would read as the membership.\",\"responses\":{\"200\":{\"description\":\"address, membership, membership_kind, count, members, problem\"},\"403\":{\"description\":\"The membership names accounts outside the domains this credential administers\"},\"404\":{\"description\":\"Unknown list\"},\"503\":{\"description\":\"The accounts could not be read\"}}}}");
   }

   HttpResponse
   RestApiServer::HandleListMembers_(const String &address, const std::vector<String> &callerDomains)
   {
      auto quote = [](const String &value) { return JsonEscapeUtf8(value); };

      const String ownDomain = StringParser::ExtractDomain(address);

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(ownDomain);

      if (!domain)
         return BuildResponse_(404, "{\"error\":\"list not found\"}");

      DistributionLists lists(domain->GetID());
      lists.Refresh();

      std::shared_ptr<DistributionList> list = lists.GetItemByAddress(address);

      if (!list)
         return BuildResponse_(404, "{\"error\":\"list not found\"}");

      const String membership = list->GetMembership();

      DistributionListMembership::Query query;
      String parseReason;
      const bool readable = DistributionListMembership::Parse(membership, query, parseReason);

      // The same question the create and the update ask of a membership before
      // they accept it: does this query reach accounts outside the domains this
      // credential administers? A confined credential is refused those forms,
      // and reading the answer to one the server administrator set is the same
      // disclosure by another door - every active address of another tenant,
      // fetched by the Control Deck the moment the list is opened. Asked before
      // anything is resolved, so nothing is read that is not answered.
      if (readable && !DistributionListMembership::WithinDomains(query, ownDomain, callerDomains))
         return BuildResponse_(403, "{\"error\":\"this list's membership names accounts outside the domains this credential administers; only a credential that reaches them may read who it delivers to\"}");

      // The rows first, in the order they are held, and then the query's
      // answers that the rows did not already name - the order the expansion
      // walks them in, so that this reads as what a message to the list does.
      AnsiString entries;
      int count = 0;

      auto append = [&entries, &count, &quote](const String &member, const char *source)
      {
         if (count > 0)
            entries += ",";

         entries += "{\"address\":\"";
         entries += quote(member);
         entries += "\",\"source\":\"";
         entries += source;
         entries += "\"}";
         count++;
      };

      std::set<String> seen;

      std::shared_ptr<DistributionListRecipients> recipients = list->GetMembers();

      if (recipients)
      {
         for (int m = 0; m < recipients->GetCount(); m++)
         {
            std::shared_ptr<DistributionListRecipient> recipient = recipients->GetItem(m);

            if (!recipient)
               continue;

            String member = recipient->GetAddress();

            if (member.IsEmpty())
               continue;

            String lowered = member;
            lowered.ToLower();

            if (seen.insert(lowered).second)
               append(member, "row");
         }
      }

      String problem;

      if (!membership.IsEmpty())
      {
         bool unavailable = false;
         const std::vector<String> answered = DistributionListMembership::Resolve(membership, list->GetDomainID(), problem, unavailable);

         // The accounts could not be read. The rows alone, answered 200 with a
         // count, would read as the membership - and this route exists to be
         // believed, because it is the only place the answer exists at all.
         if (unavailable)
            return BuildResponse_(503, "{\"error\":\"" + quote(problem) + "\"}");

         for (const String &member : answered)
         {
            if (seen.insert(member).second)
               append(member, "query");
         }
      }

      AnsiString body;
      body.Format("{\"address\":\"%hs\",\"membership\":\"%hs\",\"membership_kind\":\"%hs\",\"count\":%d,\"members\":[%hs]",
         quote(list->GetAddress()).c_str(),
         quote(membership).c_str(),
         // A text this build cannot parse is not a static list, which is what
         // the kind of a Query nobody filled in would say: a restore writes the
         // column without validating it (PreSaveLimitationsCheck returns early
         // for a restore), so the state is reachable, and calling it static
         // would contradict the membership and the problem in the same answer.
         readable ? DistributionListMembership::KindWord(query.kind) : "unreadable",
         count,
         entries.c_str());

      if (!problem.IsEmpty())
      {
         body += ",\"problem\":\"";
         body += quote(problem);
         body += "\"";
      }

      body += "}";

      return BuildResponse_(200, body);
   }
}
