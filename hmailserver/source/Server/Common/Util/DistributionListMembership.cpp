// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "DistributionListMembership.h"

#include "Parsing/StringParser.h"
#include "../Application/Application.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/DistributionList.h"
#include "../BO/DistributionListRecipients.h"
#include "../BO/DistributionListRecipient.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentDistributionList.h"
#include "../Persistence/PersistentDistributionListRecipient.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseUnavailableMarker.h"
#include "../SQL/SQLCommand.h"

#include <algorithm>
#include <set>

#include <boost/regex.hpp>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const wchar_t *const WordDomain = L"domain";
      const wchar_t *const WordEveryone = L"everyone";
      const wchar_t *const PrefixDomain = L"domain:";
      const wchar_t *const PrefixPattern = L"pattern:";

      // A domain name as the domain forms take it: something, no @, no space,
      // no wildcard. The server's own idea of a valid domain name is looser
      // than DNS - it hosts what the administrator typed - so this asks only
      // what would make the lookup meaningless.
      bool LooksLikeDomainName(const String &name)
      {
         if (name.IsEmpty() || name.GetLength() > 255)
            return false;

         for (int i = 0; i < name.GetLength(); i++)
         {
            const wchar_t c = name[i];
            if (c == L'@' || c == L' ' || c == L'*' || c == L'?' || c == L'\t' || c == L'/' || c == L'\\')
               return false;
         }

         return true;
      }

      bool HasWildcard(const String &text)
      {
         return text.Find(L'*') >= 0 || text.Find(L'?') >= 0;
      }

      String Lowered(const String &text)
      {
         String copy = text;
         copy.ToLower();
         return copy;
      }

      std::vector<String> Addresses(const SQLCommand &command, bool &failed)
      {
         std::vector<String> addresses;

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
         {
            // Not "the query answers nobody". The read did not happen - a
            // dropped connection, a table a backup has locked - and an empty
            // vector returned as though it had is how a message to a list with
            // one row and a thousand query members gets accepted with 250 and
            // delivered to the one row. Said so here, carried out through
            // Resolve, and refused at RCPT TO.
            failed = true;
            return addresses;
         }

         while (!recordset->IsEOF())
         {
            String address = recordset->GetStringValue("accountaddress");
            address.ToLower();
            if (!address.IsEmpty())
               addresses.push_back(address);
            recordset->MoveNext();
         }

         return addresses;
      }

      void SortUnique(std::vector<String> &addresses)
      {
         std::sort(addresses.begin(), addresses.end());
         addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
      }

      // The domain part of a pattern: after its last @, or empty when it has
      // none. A pattern is required to have one, so empty means malformed.
      String PatternDomain(const String &pattern)
      {
         const int at = pattern.ReverseFind(L'@');
         if (at < 0)
            return "";
         return pattern.Mid(at + 1);
      }

      // CacheContainer reads through to the database when the cache misses
      // (CacheReaderWithDbFallback), so a null domain is "this server does not
      // host it" and "the read that would have found it failed" at once, and
      // the two need different sentences - "does not host" sends an
      // administrator looking for a configuration problem that is not there -
      // and different answers at delivery. The connection pool marks the thread
      // when a read fails, so a mark that APPEARED across this one call is the
      // difference.
      //
      // Nothing is cleared here, and no Scope is opened: the marker belongs to
      // the scope SMTPConnection opened for this RCPT TO, and clearing it would
      // take away the 451 that scope exists to give.
      std::shared_ptr<const Domain> DomainByName(const String &name, bool &failed)
      {
         const bool markedBefore = DatabaseUnavailableMarker::IsMarked();
         std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(name);

         if (!domain && !markedBefore && DatabaseUnavailableMarker::IsMarked())
            failed = true;

         return domain;
      }

      std::shared_ptr<const Domain> DomainById(__int64 id, bool &failed)
      {
         const bool markedBefore = DatabaseUnavailableMarker::IsMarked();
         std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(id);

         if (!domain && !markedBefore && DatabaseUnavailableMarker::IsMarked())
            failed = true;

         return domain;
      }
   }

   const char *
   DistributionListMembership::KindWord(Kind kind)
   {
      switch (kind)
      {
      case KindOwnDomain:
         return "domain";
      case KindNamedDomain:
         return "named_domain";
      case KindPattern:
         return "pattern";
      case KindEveryone:
         return "everyone";
      case KindStatic:
      default:
         return "static";
      }
   }

   bool
   DistributionListMembership::Parse(const String &text, Query &query, String &reason)
   {
      query = Query();
      reason.Empty();

      String value = text;
      value.Trim();
      value.ToLower();

      if (value.IsEmpty())
         return true;

      if (value.GetLength() > MaxLength)
      {
         reason.Format(_T("The membership query is at most %d characters."), MaxLength);
         return false;
      }

      if (value == WordDomain)
      {
         query.kind = KindOwnDomain;
         return true;
      }

      if (value == WordEveryone)
      {
         query.kind = KindEveryone;
         return true;
      }

      if (value.StartsWith(PrefixDomain))
      {
         String name = value.Mid((int) wcslen(PrefixDomain));
         name.Trim();

         if (!LooksLikeDomainName(name))
         {
            reason = "domain: is followed by the name of a domain this server hosts, as in domain:example.com.";
            return false;
         }

         query.kind = KindNamedDomain;
         query.argument = name;
         query.scope_domain = name;
         return true;
      }

      if (value.StartsWith(PrefixPattern))
      {
         String pattern = value.Mid((int) wcslen(PrefixPattern));
         pattern.Trim();

         const String domainPart = PatternDomain(pattern);

         if (pattern.IsEmpty() || domainPart.IsEmpty() || pattern.Find(L' ') >= 0 || pattern.ReverseFind(L'@') == 0)
         {
            reason = "pattern: is followed by an address with * for any run of characters and ? for one character, as in pattern:sales-*@example.com.";
            return false;
         }

         query.kind = KindPattern;
         query.argument = pattern;
         query.scope_domain = HasWildcard(domainPart) ? String("") : domainPart;
         return true;
      }

      reason = "The membership is empty (the members are the list's rows), domain (every account of the list's domain), domain:<name> (every account of that domain), pattern:<wildcard address> or everyone.";
      return false;
   }

   bool
   DistributionListMembership::Validate(const String &text, String &normalised, String &reason)
   {
      Query query;
      if (!Parse(text, query, reason))
         return false;

      switch (query.kind)
      {
      case KindStatic:
         normalised.Empty();
         return true;

      case KindOwnDomain:
         normalised = WordDomain;
         return true;

      case KindEveryone:
         normalised = WordEveryone;
         return true;

      case KindNamedDomain:
         {
            bool failed = false;

            if (!DomainByName(query.argument, failed))
            {
               if (failed)
                  reason.Format(_T("The membership names the domain %s, which could not be read. Try again in a moment."), query.argument.c_str());
               else
                  reason.Format(_T("The membership names the domain %s, which this server does not host."), query.argument.c_str());

               return false;
            }

            normalised = String(PrefixDomain) + query.argument;
            return true;
         }

      case KindPattern:
         {
            // A pattern whose domain part is one name has to name a domain that
            // is here: accounts exist nowhere else, so any other pattern answers
            // nobody for ever, and a save that would do that should say so now.
            bool failed = false;

            if (!query.scope_domain.IsEmpty() && !DomainByName(query.scope_domain, failed))
            {
               if (failed)
                  reason.Format(_T("The membership pattern names the domain %s, which could not be read. Try again in a moment."), query.scope_domain.c_str());
               else
                  reason.Format(_T("The membership pattern names the domain %s, which this server does not host."), query.scope_domain.c_str());

               return false;
            }

            normalised = String(PrefixPattern) + query.argument;
            return true;
         }
      }

      return false;
   }

   bool
   DistributionListMembership::WithinDomains(const Query &query, const String &ownDomain, const std::vector<String> &callerDomains)
   {
      if (callerDomains.empty())
         return true;

      switch (query.kind)
      {
      case KindStatic:
      case KindOwnDomain:
         return true;

      case KindEveryone:
         return false;

      case KindNamedDomain:
      case KindPattern:
      default:
         break;
      }

      // A pattern whose domain part is a wildcard reaches every domain.
      if (query.scope_domain.IsEmpty())
         return false;

      if (query.scope_domain.CompareNoCase(ownDomain.c_str()) == 0)
         return true;

      for (const String &allowed : callerDomains)
      {
         if (allowed.CompareNoCase(query.scope_domain.c_str()) == 0)
            return true;
      }

      return false;
   }

   std::vector<String>
   DistributionListMembership::AccountsOfDomain(__int64 domainId, bool &failed)
   {
      SQLCommand command("select accountaddress from hm_accounts where accountdomainid = @DOMAINID and accountactive = 1 order by accountaddress asc");
      command.AddParameter("@DOMAINID", domainId);
      return Addresses(command, failed);
   }

   std::vector<String>
   DistributionListMembership::AccountsOfEveryDomain(bool &failed)
   {
      SQLCommand command("select a.accountaddress from hm_accounts a inner join hm_domains d on d.domainid = a.accountdomainid where a.accountactive = 1 and d.domainactive = 1 order by a.accountaddress asc");
      return Addresses(command, failed);
   }

   std::vector<String>
   DistributionListMembership::AccountsMatching(const String &pattern, const String &scopeDomain, bool &failed)
   {
      std::vector<String> candidates;

      if (scopeDomain.IsEmpty())
      {
         candidates = AccountsOfEveryDomain(failed);
      }
      else
      {
         std::shared_ptr<const Domain> domain = DomainByName(scopeDomain, failed);
         if (!domain || !domain->GetIsActive())
            return candidates;

         candidates = AccountsOfDomain(domain->GetID(), failed);
      }

      std::vector<String> matching;

      // One regular expression for the pattern, compiled once and asked of
      // every candidate. StringParser::WildcardMatchNoCase builds and throws
      // away a boost::wregex per call, which is nothing for the handful of
      // comparisons the rest of the server makes with it and is one compile
      // per mailbox here - a domain of five thousand accounts, on every RCPT
      // TO, from anybody who can reach a public list.
      String lowered = pattern;
      lowered.ToLower();

      boost::wregex expression;

      try
      {
         expression.assign(StringParser::WildcardToRegularExpression(lowered));
      }
      catch (std::runtime_error &)
      {
         // A pattern that will not compile answers nobody, which is what
         // TestExactMatch answers for it, rather than throwing out of a
         // delivery. The candidates were read, so this is a query that answers
         // nobody and not a failure to answer it.
         return std::vector<String>();
      }

      for (const String &address : candidates)
      {
         try
         {
            if (boost::regex_match(address, expression))
               matching.push_back(address);
         }
         catch (std::runtime_error &)
         {
            // Per address, as WildcardMatch is: boost gives up on a match whose
            // state count runs away, and that is an answer of "no" for this one
            // address rather than the end of the scan.
         }
      }

      return matching;
   }

   std::vector<String>
   DistributionListMembership::Resolve(const Query &query, __int64 ownDomainId, String &problem, bool &unavailable)
   {
      problem.Empty();
      unavailable = false;
      std::vector<String> addresses;

      switch (query.kind)
      {
      case KindStatic:
         return addresses;

      case KindOwnDomain:
         {
            std::shared_ptr<const Domain> domain = DomainById(ownDomainId, unavailable);
            if (!domain)
            {
               problem = "The list's own domain could not be read.";
               unavailable = true;
               return addresses;
            }

            if (!domain->GetIsActive())
            {
               problem = "The list's domain is disabled.";
               return addresses;
            }

            addresses = AccountsOfDomain(domain->GetID(), unavailable);
            break;
         }

      case KindNamedDomain:
         {
            std::shared_ptr<const Domain> domain = DomainByName(query.argument, unavailable);
            if (!domain)
            {
               if (unavailable)
                  problem.Format(_T("The membership names the domain %s, which could not be read."), query.argument.c_str());
               else
                  problem.Format(_T("The membership names the domain %s, which this server does not host."), query.argument.c_str());

               return addresses;
            }

            if (!domain->GetIsActive())
            {
               problem.Format(_T("The domain %s the membership names is disabled."), query.argument.c_str());
               return addresses;
            }

            addresses = AccountsOfDomain(domain->GetID(), unavailable);
            break;
         }

      case KindPattern:
         if (!query.scope_domain.IsEmpty())
         {
            std::shared_ptr<const Domain> domain = DomainByName(query.scope_domain, unavailable);
            if (!domain)
            {
               if (unavailable)
                  problem.Format(_T("The membership pattern names the domain %s, which could not be read."), query.scope_domain.c_str());
               else
                  problem.Format(_T("The membership pattern names the domain %s, which this server does not host."), query.scope_domain.c_str());

               return addresses;
            }

            if (!domain->GetIsActive())
            {
               problem.Format(_T("The domain %s the membership pattern names is disabled."), query.scope_domain.c_str());
               return addresses;
            }
         }

         addresses = AccountsMatching(query.argument, query.scope_domain, unavailable);
         break;

      case KindEveryone:
         addresses = AccountsOfEveryDomain(unavailable);
         break;
      }

      if (unavailable)
      {
         // Whatever came back is half of an answer at best. Emptied, so that a
         // caller that reads the addresses and ignores the flag delivers to
         // nobody rather than to a subset it would present as the membership.
         addresses.clear();

         if (problem.IsEmpty())
            problem = "The server's accounts could not be read, so the membership query could not be answered.";

         return addresses;
      }

      SortUnique(addresses);
      return addresses;
   }

   std::vector<String>
   DistributionListMembership::Resolve(const String &text, __int64 ownDomainId, String &problem, bool &unavailable)
   {
      Query query;
      String reason;
      unavailable = false;

      if (!Parse(text, query, reason))
      {
         // A row written by hand, or by a restore of a backup from a newer
         // server: not a query this server knows, so nobody, and the reason is
         // the problem. Not unavailable - the query WAS read, and it is this
         // server that cannot make sense of it.
         problem = reason;
         return std::vector<String>();
      }

      return Resolve(query, ownDomainId, problem, unavailable);
   }

   std::vector<String>
   DistributionListMembership::Members(std::shared_ptr<const DistributionList> list, bool &unavailable)
   {
      std::vector<String> members;
      std::set<String> seen;

      unavailable = false;

      std::shared_ptr<const DistributionListRecipients> rows = list->GetMembers();
      // owned by this call: DistributionList::GetMembers() loads a new collection for every caller
      const std::vector<std::shared_ptr<DistributionListRecipient> > recipients = rows->GetConstVector();
      for (const std::shared_ptr<DistributionListRecipient> &recipient : recipients)
      {
         const String address = recipient->GetAddress();
         if (address.IsEmpty())
            continue;

         if (seen.insert(Lowered(address)).second)
            members.push_back(address);
      }

      const String membership = list->GetMembership();
      if (membership.IsEmpty())
         return members;

      String problem;
      const std::vector<String> answered = Resolve(membership, list->GetDomainID(), problem, unavailable);

      if (unavailable)
      {
         // The database did not answer. Reported under its own code and handed
         // back as a failure rather than as a membership: the caller refuses
         // the recipient, which the sender sees as a temporary failure and
         // retries. Delivering to the rows here would be the silent half-
         // delivery this flag exists to stop - a 250 for a message that reached
         // one address of a thousand, which nothing ever retries.
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6982, "DistributionListMembership::Members",
            Formatter::Format("The membership query of distribution list {0} ({1}) could not be answered because the database did not answer: {2} The recipient was refused with a temporary failure rather than delivered to in part.",
               list->GetAddress(), membership, problem));

         return std::vector<String>();
      }

      if (!problem.IsEmpty())
      {
         // Reported, because the administrator asked for members the server
         // cannot find and a posting is about to reach only the rows. The
         // static rows are still delivered to: a query that answers nobody
         // is a list that is smaller than it was, not a refused message.
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6980, "DistributionListMembership::Members",
            Formatter::Format("The membership query of distribution list {0} ({1}) could not be answered: {2} The message is delivered to the list's rows alone.",
               list->GetAddress(), membership, problem));
      }

      for (const String &address : answered)
      {
         if (seen.insert(address).second)
            members.push_back(address);
      }

      return members;
   }

   bool
   DistributionListMembership::IsMember(std::shared_ptr<const DistributionList> list, const String &address)
   {
      String wanted = address;
      wanted.Trim();
      wanted.ToLower();
      if (wanted.IsEmpty())
         return false;

      std::shared_ptr<const DistributionListRecipients> rows = list->GetMembers();
      // owned by this call: DistributionList::GetMembers() loads a new collection for every caller
      const std::vector<std::shared_ptr<DistributionListRecipient> > recipients = rows->GetConstVector();
      for (const std::shared_ptr<DistributionListRecipient> &recipient : recipients)
      {
         if (Lowered(recipient->GetAddress()) == wanted)
            return true;
      }

      const String membership = list->GetMembership();
      if (membership.IsEmpty())
         return false;

      Query query;
      String reason;
      if (!Parse(membership, query, reason))
         return false;

      // The query answers accounts and nothing else, so the question is
      // whether this address is an active account the query would answer -
      // asked of the cache rather than by resolving the whole query, since
      // this is the sender check at RCPT TO.
      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(wanted);
      if (!account || !account->GetActive())
         return false;

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(account->GetDomainID());
      if (!domain || !domain->GetIsActive())
         return false;

      switch (query.kind)
      {
      case KindOwnDomain:
         return account->GetDomainID() == list->GetDomainID();

      case KindNamedDomain:
         return domain->GetName().CompareNoCase(query.argument.c_str()) == 0;

      case KindPattern:
         return StringParser::WildcardMatchNoCase(query.argument, wanted);

      case KindEveryone:
         return true;

      case KindStatic:
      default:
         return false;
      }
   }

   bool
   DistributionListMembership::RenameDomain(const String &oldName, const String &newName, String &reason)
   {
      reason.Empty();

      const String oldLower = Lowered(oldName);
      const String newLower = Lowered(newName);
      if (oldLower.IsEmpty() || newLower.IsEmpty() || oldLower == newLower)
         return true;

      struct Row
      {
         __int64 id;
         String text;
      };
      std::vector<Row> rows;

      {
         SQLCommand command("select distributionlistid, distributionlistmembership from hm_distributionlists where distributionlistmembership <> ''");
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
         {
            reason = "The distribution lists could not be read.";
            return false;
         }

         while (!recordset->IsEOF())
         {
            Row row;
            row.id = recordset->GetInt64Value("distributionlistid");
            row.text = recordset->GetStringValue("distributionlistmembership");
            rows.push_back(row);
            recordset->MoveNext();
         }
      }

      for (const Row &row : rows)
      {
         Query query;
         String parseReason;
         if (!Parse(row.text, query, parseReason))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6981, "DistributionListMembership::RenameDomain",
               Formatter::Format("Distribution list {0} has a membership query this server cannot read ({1}); it was left as it is by the rename of {2}.",
                  StringParser::IntToString(row.id), row.text, oldName));
            continue;
         }

         String rewritten;

         if (query.kind == KindNamedDomain && query.argument == oldLower)
            rewritten = String(PrefixDomain) + newLower;
         else if (query.kind == KindPattern && !query.scope_domain.IsEmpty() && query.scope_domain == oldLower)
         {
            const int at = query.argument.ReverseFind(L'@');
            rewritten = String(PrefixPattern) + query.argument.Left(at + 1) + newLower;
         }
         else
            continue;

         if (rewritten.GetLength() > MaxLength)
         {
            // A rename to a longer name can push a pattern past the column.
            // Nothing else would catch it: CheckLimitations returns early for
            // a rename, so Validate - the one place the length is tested -
            // never runs on this save, and the database would decide instead:
            // a truncation error that fails the rename after the domain, its
            // accounts and its aliases have already been renamed, or, on a
            // MySQL that is not strict, a silently shortened query nobody
            // wrote. Left as it is and named in the log: it goes on naming the
            // old domain, answers nobody, and says so at the next delivery.
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6981, "DistributionListMembership::RenameDomain",
               Formatter::Format("Distribution list {0} has a membership query that the rename of {1} to {2} would make longer than the {3} characters the column holds; it was left naming the old domain.",
                  StringParser::IntToString(row.id), oldName, newName, StringParser::IntToString(MaxLength)));
            continue;
         }

         std::shared_ptr<DistributionList> list(new DistributionList);
         if (!PersistentDistributionList::ReadObject(list, row.id))
            continue;

         list->SetMembership(rewritten);

         String saveError;
         if (!PersistentDistributionList::SaveObject(list, saveError, PersistenceModeRename))
         {
            reason = "The domain could not be renamed because the membership query of distribution list '" + list->GetAddress() + "' could not be updated.";
            return false;
         }

         LOG_APPLICATION("Distribution list " + list->GetAddress() + ": membership query " + row.text + " follows the rename of " + oldName + " to " + newName + " as " + rewritten + ".");
      }

      return true;
   }
}
