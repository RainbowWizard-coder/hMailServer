// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// eDiscovery over REST (docs/Discovery.md): the cases, the searches run for them,
// the holds they place and the exports they make.
//
//    GET    /api/v1/discovery/cases                                 every case, summarised
//    POST   /api/v1/discovery/cases                                 a case opened
//    GET    /api/v1/discovery/cases/{id}                            a case, its searches, holds and exports
//    POST   /api/v1/discovery/cases/{id}/close                      closed, its holds released
//    POST   /api/v1/discovery/cases/{id}/searches                   a search queued
//    GET    /api/v1/discovery/cases/{id}/searches/{search}          its state and a page of its hits
//    POST   /api/v1/discovery/cases/{id}/searches/{search}/export   an export of its hits queued
//    POST   /api/v1/discovery/cases/{id}/holds                      an account, a domain, or the archived copies a search matched, held
//    DELETE /api/v1/discovery/cases/{id}/holds/{hold}               released
//    GET    /api/v1/discovery/cases/{id}/exports/{export}           the zip
//
// WHO. view_discovery reads, and the download needs view_mailbox beside it;
// manage_discovery writes (PermissionFor_, SecondPermissionFor_). A search that
// reads the archive - scope archive or both - needs view_archive as well, to
// run, to read and to download, since its hits are archived copies
// (CallerMayReadArchive_). No path names a domain, so a credential confined to
// named domains is confined here: every custodian it names and every account
// or domain it holds must be inside its domains - "*", every mailbox, is
// refused it - and a case with a search or a hold reaching outside them, or an
// empty case somebody else opened, is the same 404 as a case that is not there.
//
// THE AUDIT TRAIL. A case, a search, a hold and an export are rows of audited
// tables, recorded under the request's actor by the statement chokepoint with
// the words DiscoveryStore supplies - the search with its whole query, which is
// why a query is refused when it is longer than the trail keeps. The download
// writes no row, so it records its own event. All of it while the audit trail
// is on (audit_trail_enabled, on by default); the case's own rows record every
// search's query and who ran it, every hold and every export, and who, whatever
// that switch says - the download alone is only on the trail.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "FileUtilities.h"
#include "AuditTrail.h"
#include "DiscoveryStore.h"
#include "DiscoveryHolds.h"
#include "DiscoverySearch.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentArchiveIndex.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <climits>
#include <iterator>
#include <set>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A query is recorded whole on the audit trail, whose detail is 2,000
      // characters; the rest of the row's words take the difference.
      const int MaxQueryLength = 1800;

      // The archived copies one request may put on hold.
      const size_t MaxArchiveHolds = 10000;

      // A download is read into memory once, as ArchiveDownload_'s is.
      const unsigned __int64 DownloadCeiling = 500 * 1024 * 1024;

      AnsiString Quoted(const String &value)
      {
         return "\"" + RestApiServer::JsonEscapeUtf8(value) + "\"";
      }

      AnsiString Number(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      AnsiString Bool(bool value)
      {
         return value ? AnsiString("true") : AnsiString("false");
      }

      const char *StateWord(int state)
      {
         switch (state)
         {
         case DiscoveryStore::JobQueued: return "queued";
         case DiscoveryStore::JobRunning: return "running";
         case DiscoveryStore::JobDone: return "done";
         default: return "failed";
         }
      }

      const char *ScopeWord(int scope)
      {
         switch (scope)
         {
         case DiscoveryHolds::ScopeAccount: return "account";
         case DiscoveryHolds::ScopeDomain: return "domain";
         default: return "archive";
         }
      }

      bool DomainIn(const std::vector<String> &domains, const String &domain)
      {
         if (domains.empty())
            return true;

         if (domain.IsEmpty())
            return false;

         for (const String &allowed : domains)
         {
            if (allowed.CompareNoCase(domain.c_str()) == 0)
               return true;
         }

         return false;
      }

      String DomainOfCustodian(const String &custodian)
      {
         return custodian.Find(_T("@")) >= 0 ? StringParser::ExtractDomain(custodian) : custodian;
      }

      // Every custodian inside the credential's domains; "*" is never.
      bool CustodiansAllowed(const std::vector<String> &domains, const std::vector<String> &custodians)
      {
         if (domains.empty())
            return true;

         for (const String &custodian : custodians)
         {
            if (custodian == _T("*") || !DomainIn(domains, DomainOfCustodian(custodian)))
               return false;
         }

         return true;
      }

      bool SearchAllowed(const std::vector<String> &domains, const DiscoveryStore::Search &search)
      {
         if (domains.empty())
            return true;

         DiscoverySearch::Query query;
         String problem;
         if (!DiscoverySearch::ParseQuery(search.query, query, problem))
            return false;

         return CustodiansAllowed(domains, query.custodians);
      }

      bool HoldAllowed(const std::vector<String> &domains, const DiscoveryStore::Hold &hold)
      {
         if (domains.empty())
            return true;

         if (hold.scope == DiscoveryHolds::ScopeAccount)
            return DomainIn(domains, StringParser::ExtractDomain(hold.name));

         if (hold.scope == DiscoveryHolds::ScopeDomain)
            return DomainIn(domains, hold.name);

         PersistentArchiveIndex::Entry entry;
         if (PersistentArchiveIndex::Get(hold.owner_id, entry))
            return DomainIn(domains, entry.domain);

         // The copy is gone - swept or erased since the hold was released, or
         // the index could not be read - and the hold's name still says whose
         // it was: "<address> (archive #<id>)", or "inbound (...)", which names
         // no domain and so is inside no confined credential's.
         String name = hold.name;
         const int bracket = name.Find(_T(" (archive #"));
         if (bracket >= 0)
            name = name.Mid(0, bracket);

         return name.Find(_T("@")) > 0 && DomainIn(domains, StringParser::ExtractDomain(name));
      }

      // Whether a search reads the archive: its scope is archive or both.
      bool SearchReachesArchive(const DiscoveryStore::Search &search)
      {
         DiscoverySearch::Query query;
         String problem;

         // A query that no longer reads is treated as reaching everything.
         if (!DiscoverySearch::ParseQuery(search.query, query, problem))
            return true;

         return query.archive;
      }

      // A confined credential sees a case whose every search and hold is inside
      // its domains, and no other. A case with neither - just opened - it sees
      // only when it opened the case itself: "every search and hold is inside"
      // is true of an empty case for every credential, and a case's name and
      // description ("Smith v. Jones") are not every tenant's to read, nor its
      // close every tenant's to make.
      bool CaseVisible(const std::vector<String> &domains, const String &actor, const DiscoveryStore::Case &theCase)
      {
         if (domains.empty())
            return true;

         std::vector<DiscoveryStore::Search> searches;
         std::vector<DiscoveryStore::Hold> holds;
         if (!DiscoveryStore::ListSearches(theCase.id, searches) || !DiscoveryStore::ListHolds(theCase.id, holds))
            return false;

         if (searches.empty() && holds.empty())
            return !actor.IsEmpty() && theCase.created_by.CompareNoCase(actor.c_str()) == 0;

         for (const DiscoveryStore::Search &search : searches)
         {
            if (!SearchAllowed(domains, search))
               return false;
         }

         for (const DiscoveryStore::Hold &hold : holds)
         {
            if (!HoldAllowed(domains, hold))
               return false;
         }

         return true;
      }

      AnsiString SearchFields(const DiscoveryStore::Search &search)
      {
         AnsiString json = "\"id\":" + Number(search.id);
         json += ",\"case\":" + Number(search.case_id);
         json += ",\"state\":\"" + AnsiString(StateWord(search.state)) + "\"";
         json += ",\"query\":" + (search.query.IsEmpty() ? AnsiString("null") : search.query);
         json += ",\"ran_by\":" + Quoted(search.ran_by);
         json += ",\"ran\":" + Number(search.ran);
         json += ",\"finished\":" + Number(search.finished);
         json += ",\"scanned\":" + Number(search.scanned);
         json += ",\"matched\":" + Number(search.matched);
         json += ",\"unsearched\":" + Number(search.unsearched);
         json += ",\"truncated\":" + Bool(search.truncated);
         json += ",\"error\":" + Quoted(search.error);
         return json;
      }

      AnsiString HoldJson(const DiscoveryStore::Hold &hold)
      {
         AnsiString json = "{\"id\":" + Number(hold.id);
         json += ",\"case\":" + Number(hold.case_id);
         json += ",\"scope\":\"" + AnsiString(ScopeWord(hold.scope)) + "\"";
         json += ",\"owner_id\":" + Number(hold.owner_id);
         json += ",\"name\":" + Quoted(hold.name);
         json += ",\"placed_by\":" + Quoted(hold.placed_by);
         json += ",\"placed\":" + Number(hold.placed);
         json += ",\"released_by\":" + Quoted(hold.released_by);
         json += ",\"released\":" + Number(hold.released);
         json += ",\"live\":" + Bool(hold.Live());
         json += "}";
         return json;
      }

      AnsiString ExportJson(const DiscoveryStore::Export &result)
      {
         AnsiString json = "{\"id\":" + Number(result.id);
         json += ",\"case\":" + Number(result.case_id);
         json += ",\"search\":" + Number(result.search_id);
         json += ",\"state\":\"" + AnsiString(StateWord(result.state)) + "\"";
         json += ",\"file\":" + Quoted(result.file);
         json += ",\"size\":" + Number(result.size);
         json += ",\"sha256\":" + Quoted(result.sha256);
         json += ",\"items\":" + Number(result.items);
         json += ",\"withheld\":" + Number(result.withheld);
         json += ",\"requested_by\":" + Quoted(result.requested_by);
         json += ",\"requested\":" + Number(result.requested);
         json += ",\"finished\":" + Number(result.finished);
         json += ",\"error\":" + Quoted(result.error);
         json += ",\"download\":\"/api/v1/discovery/cases/" + Number(result.case_id) + "/exports/" + Number(result.id) + "\"";
         json += "}";
         return json;
      }

      // A case: summarised for the listing, whole - its searches, holds and
      // exports - for its own route.
      AnsiString CaseJson(const DiscoveryStore::Case &theCase, bool whole)
      {
         std::vector<DiscoveryStore::Search> searches;
         std::vector<DiscoveryStore::Hold> holds;
         std::vector<DiscoveryStore::Export> exports;
         DiscoveryStore::ListSearches(theCase.id, searches);
         DiscoveryStore::ListHolds(theCase.id, holds);
         DiscoveryStore::ListExports(theCase.id, exports);

         int liveHolds = 0;
         for (const DiscoveryStore::Hold &hold : holds)
         {
            if (hold.Live())
               liveHolds++;
         }

         AnsiString json = "{\"id\":" + Number(theCase.id);
         json += ",\"name\":" + Quoted(theCase.name);
         json += ",\"description\":" + Quoted(theCase.description);
         json += ",\"state\":\"" + AnsiString(theCase.state == DiscoveryStore::CaseClosed ? "closed" : "open") + "\"";
         json += ",\"created_by\":" + Quoted(theCase.created_by);
         json += ",\"created\":" + Number(theCase.created);
         json += ",\"closed_by\":" + Quoted(theCase.closed_by);
         json += ",\"closed\":" + Number(theCase.closed);
         json += ",\"search_count\":" + Number((__int64) searches.size());
         json += ",\"live_holds\":" + Number(liveHolds);
         json += ",\"export_count\":" + Number((__int64) exports.size());

         if (whole)
         {
            json += ",\"searches\":[";
            for (size_t i = 0; i < searches.size(); i++)
               json += (i > 0 ? AnsiString(",{") : AnsiString("{")) + SearchFields(searches[i]) + "}";

            json += "],\"holds\":[";
            for (size_t i = 0; i < holds.size(); i++)
               json += (i > 0 ? AnsiString(",") : AnsiString()) + HoldJson(holds[i]);

            json += "],\"exports\":[";
            for (size_t i = 0; i < exports.size(); i++)
               json += (i > 0 ? AnsiString(",") : AnsiString()) + ExportJson(exports[i]);

            json += "]";
         }

         json += "}";
         return json;
      }

      bool HasControl(const String &text, bool allowLines)
      {
         for (int i = 0; i < text.GetLength(); i++)
         {
            const wchar_t c = text[i];
            if (c == 0x7f)
               return true;

            if (c < 0x20 && !(allowLines && (c == '\n' || c == '\t')))
               return true;
         }

         return false;
      }

      String Wide(const std::string &utf8)
      {
         String out;
         Unicode::MultiByteToWide(AnsiString(utf8.c_str()), out);
         return out;
      }
   }

   void
   RestApiServer::ParseDiscoveryRoute_(const AnsiString &method, const AnsiString &path, Route &route)
   {
      const AnsiString prefix = "/api/v1/discovery/cases";

      if (path == prefix)
      {
         if (method == "GET")
            route.kind = RouteDiscoveryCaseList;
         else if (method == "POST")
            route.kind = RouteDiscoveryCaseCreate;

         return;
      }

      if (path.GetLength() <= prefix.GetLength() + 1)
         return;

      std::vector<AnsiString> segments = StringParser::SplitString(path.Mid(prefix.GetLength() + 1), AnsiString("/"));

      auto isNumber = [](const AnsiString &segment)
      {
         if (segment.IsEmpty() || segment.GetLength() > 18)
            return false;
         for (int i = 0; i < segment.GetLength(); i++)
         {
            if (segment.GetAt(i) < '0' || segment.GetAt(i) > '9')
               return false;
         }
         return true;
      };

      if (segments.empty() || !isNumber(segments[0]))
         return;

      route.record_id = _atoi64(segments[0].c_str());

      if (segments.size() == 1)
      {
         if (method == "GET")
            route.kind = RouteDiscoveryCaseGet;
         return;
      }

      if (segments.size() == 2)
      {
         if (method == "POST" && segments[1] == "close")
            route.kind = RouteDiscoveryCaseClose;
         else if (method == "POST" && segments[1] == "searches")
            route.kind = RouteDiscoverySearchCreate;
         else if (method == "POST" && segments[1] == "holds")
            route.kind = RouteDiscoveryHoldCreate;
         return;
      }

      if (!isNumber(segments[2]))
         return;

      route.message_id = _atoi64(segments[2].c_str());

      if (segments.size() == 3)
      {
         if (method == "GET" && segments[1] == "searches")
            route.kind = RouteDiscoverySearchGet;
         else if (method == "DELETE" && segments[1] == "holds")
            route.kind = RouteDiscoveryHoldRelease;
         else if (method == "GET" && segments[1] == "exports")
            route.kind = RouteDiscoveryExportGet;
         return;
      }

      if (segments.size() == 4 && method == "POST" && segments[1] == "searches" && segments[3] == "export")
         route.kind = RouteDiscoveryExportCreate;
   }

   HttpResponse
   RestApiServer::HandleDiscovery_(const Caller &caller, const Route &route, const AnsiString &requestBody)
   {
      // Anything queued - by this request or before the server last stopped -
      // is taken up; a runner already at work is left to it.
      DiscoverySearch::Kick();

      auto refuse = [](int status, const String &sentence)
      {
         return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(Utf8_(sentence)) + "\"}");
      };

      const String by = AuditActorFor_(caller).name;

      if (route.kind == RouteDiscoveryCaseList)
      {
         std::vector<DiscoveryStore::Case> cases;
         if (!DiscoveryStore::ListCases(cases))
            return refuse(500, _T("The cases could not be read."));

         AnsiString json = "{\"cases\":[";
         bool first = true;
         for (const DiscoveryStore::Case &theCase : cases)
         {
            if (!CaseVisible(caller.domains, by, theCase))
               continue;

            if (!first)
               json += ",";
            first = false;
            json += CaseJson(theCase, false);
         }
         json += "]}";

         return BuildResponse_(200, json);
      }

      if (route.kind == RouteDiscoveryCaseCreate)
      {
         JsonValue document;
         std::string parseError;
         if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), document, parseError) || !document.IsObject())
            return refuse(400, _T("The body must be a JSON object: {name, description}."));

         String name, description;
         for (const auto &member : document.Members())
         {
            if (member.first == "name" || member.first == "description")
            {
               if (member.second.IsNull())
                  continue;

               if (!member.second.IsString())
                  return refuse(400, Formatter::Format("{0} must be a string.", Wide(member.first)));

               if (member.first == "name")
                  name = Wide(member.second.AsString());
               else
                  description = Wide(member.second.AsString());

               continue;
            }

            return refuse(400, Formatter::Format("{0} is not a field of a case: the fields are name and description.", Wide(member.first)));
         }

         name.Trim();
         description.Trim();

         if (name.IsEmpty() || name.GetLength() > 255 || HasControl(name, false))
            return refuse(400, _T("A case needs a name of 1 to 255 characters, on one line."));

         if (description.GetLength() > 1000 || HasControl(description, true))
            return refuse(400, _T("A case's description is at most 1,000 characters, with no control character but a line break or a tab."));

         const __int64 id = DiscoveryStore::CreateCase(name, description, by);
         if (id <= 0)
            return refuse(500, _T("The case could not be written."));

         DiscoveryStore::Case created;
         if (!DiscoveryStore::GetCase(id, created))
            return refuse(500, _T("The case was written and could not be read back."));

         LOG_APPLICATION("RestApi: Discovery case " + StringParser::IntToString(id) + " opened by " + by + ".");
         return BuildResponse_(201, CaseJson(created, true));
      }

      // Everything else is inside one case, which a confined credential that
      // may not see it finds as absent as a case that is not there.
      DiscoveryStore::Case theCase;
      if (!DiscoveryStore::GetCase(route.record_id, theCase) || !CaseVisible(caller.domains, by, theCase))
         return refuse(404, _T("There is no such case."));

      const bool caseOpen = theCase.state == DiscoveryStore::CaseOpen;

      switch (route.kind)
      {
      case RouteDiscoveryCaseGet:
         return BuildResponse_(200, CaseJson(theCase, true));

      case RouteDiscoveryCaseClose:
      {
         if (!caseOpen)
            return refuse(409, _T("The case is closed already."));

         int released = 0;
         if (!DiscoveryStore::CloseCase(theCase, by, released))
            return refuse(500, _T("The case could not be closed."));

         DiscoveryStore::Case closed;
         DiscoveryStore::GetCase(theCase.id, closed);

         LOG_APPLICATION("RestApi: Discovery case " + StringParser::IntToString(theCase.id) + " closed by " + by + "; " +
                         StringParser::IntToString(released) + " hold(s) released with it.");

         AnsiString json = CaseJson(closed, true);
         json = json.Mid(0, json.GetLength() - 1) + ",\"released\":" + Number(released) + "}";
         return BuildResponse_(200, json);
      }

      case RouteDiscoverySearchCreate:
      {
         if (!caseOpen)
            return refuse(409, _T("The case is closed: a closed case runs no search. Open a new one."));

         DiscoverySearch::Query query;
         String problem;
         if (!DiscoverySearch::ParseQuery(requestBody, query, problem))
            return refuse(400, problem);

         if (!CustodiansAllowed(caller.domains, query.custodians))
         {
            for (const String &custodian : query.custodians)
            {
               if (custodian == _T("*"))
                  return BuildForbiddenResponse_("this credential is restricted to named domains, and \"*\" is every mailbox on the server");
            }

            return BuildForbiddenResponse_("this credential is not permitted for that domain");
         }

         // The archive's copies are view_archive's to read (RouteArchiveGet),
         // so a search of them is too: its hits and its export hand them over.
         // The default scope is both, so a role without it names mailboxes.
         if (query.archive && !CallerMayReadArchive_(caller))
            return BuildForbiddenResponse_("a search of the archive needs view_archive as well; this role may search with scope mailboxes");

         const AnsiString json = DiscoverySearch::QueryJson(query);
         if (json.GetLength() > MaxQueryLength)
            return refuse(400, _T("The query is longer than the audit trail records whole (1,800 characters). Run it as two searches."));

         const __int64 id = DiscoveryStore::CreateSearch(theCase, json, by);
         if (id <= 0)
            return refuse(500, _T("The search could not be written."));

         DiscoverySearch::Kick();

         AnsiString body = "{\"id\":" + Number(id) + ",\"case\":" + Number(theCase.id) + ",\"state\":\"queued\",\"query\":" + json +
                           ",\"status\":\"/api/v1/discovery/cases/" + Number(theCase.id) + "/searches/" + Number(id) + "\"}";
         return BuildResponse_(202, body);
      }

      case RouteDiscoverySearchGet:
      {
         DiscoveryStore::Search search;
         if (!DiscoveryStore::GetSearch(route.message_id, search) || search.case_id != theCase.id)
            return refuse(404, _T("There is no such search in this case."));

         int offset = atoi(QueryParameter_(route.query, "offset").c_str());
         if (offset < 0)
            offset = 0;

         const AnsiString limitText = QueryParameter_(route.query, "limit");
         int limit = limitText.IsEmpty() ? 100 : atoi(limitText.c_str());
         if (limit < 1)
            limit = 1;
         if (limit > 1000)
            limit = 1000;

         // What the search could not read is paged on its own, since there may
         // be up to 10,000 of it listed beside the hits.
         int unsearchedOffset = atoi(QueryParameter_(route.query, "unsearched_offset").c_str());
         if (unsearchedOffset < 0)
            unsearchedOffset = 0;

         const AnsiString unsearchedLimitText = QueryParameter_(route.query, "unsearched_limit");
         int unsearchedLimit = unsearchedLimitText.IsEmpty() ? 100 : atoi(unsearchedLimitText.c_str());
         if (unsearchedLimit < 0)
            unsearchedLimit = 0;
         if (unsearchedLimit > 1000)
            unsearchedLimit = 1000;

         // A search of the archive lists archived copies, which a role without
         // view_archive does not read anywhere else (docs/Discovery.md).
         if (SearchReachesArchive(search) && !CallerMayReadArchive_(caller))
            return BuildForbiddenResponse_("this search reads the archive, and reading what it found needs view_archive as well");

         std::vector<AnsiString> hits;
         std::vector<AnsiString> unsearched;
         int total = search.matched;
         int unsearchedListed = 0;

         if (search.state == DiscoveryStore::JobDone && !search.result_file.IsEmpty())
         {
            const String path = FileUtilities::Combine(DiscoveryStore::CaseDirectory(theCase.id), search.result_file);
            if (!DiscoverySearch::ReadResults(path, offset, limit, hits, total, unsearchedOffset, unsearchedLimit, unsearched, unsearchedListed))
               return refuse(500, _T("The search's results file could not be read: ") + path);
         }

         AnsiString json = "{" + SearchFields(search);
         json += ",\"offset\":" + Number(offset);
         json += ",\"limit\":" + Number(limit);
         json += ",\"total\":" + Number(total);
         json += ",\"hits\":[";
         for (size_t i = 0; i < hits.size(); i++)
            json += (i > 0 ? AnsiString(",") : AnsiString()) + hits[i];
         json += "],\"unsearched_offset\":" + Number(unsearchedOffset);
         json += ",\"unsearched_limit\":" + Number(unsearchedLimit);
         json += ",\"unsearched_listed\":" + Number(unsearchedListed);
         json += ",\"unsearched_list\":[";
         for (size_t i = 0; i < unsearched.size(); i++)
            json += (i > 0 ? AnsiString(",") : AnsiString()) + unsearched[i];
         json += "]}";

         return BuildResponse_(200, json);
      }

      case RouteDiscoveryHoldCreate:
      {
         if (!caseOpen)
            return refuse(409, _T("The case is closed: a closed case places no hold. Open a new one."));

         JsonValue document;
         std::string parseError;
         if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), document, parseError) || !document.IsObject())
            return refuse(400, _T("The body must be a JSON object: {scope: account or domain, target}, or {scope: archive, search}."));

         std::string scope;
         String target;
         __int64 searchId = 0;

         for (const auto &member : document.Members())
         {
            if (member.first == "scope" && member.second.IsString())
               scope = member.second.AsString();
            else if (member.first == "target" && member.second.IsString())
               target = Wide(member.second.AsString());
            else if (member.first == "search" && member.second.IsNumber())
               searchId = (__int64) member.second.AsNumber();
            else
               return refuse(400, Formatter::Format("{0} is not a field of a hold, or not of its type: the fields are scope (account, domain or archive), target (a string) and search (a number).", Wide(member.first)));
         }

         target.Trim();
         target.ToLower();

         std::vector<DiscoveryStore::Hold> existing;
         DiscoveryStore::ListHolds(theCase.id, existing);

         if (scope == "account" || scope == "domain")
         {
            const bool account = scope == "account";
            if (target.IsEmpty())
               return refuse(400, _T("A hold on an account or a domain names it in target."));

            // The credential's reach first, so that the refusal says nothing
            // about whether the name exists outside it.
            if (!DomainIn(caller.domains, account ? StringParser::ExtractDomain(target) : target))
               return BuildForbiddenResponse_("this credential is not permitted for that domain");

            __int64 ownerId = 0;
            String name;

            if (account)
            {
               std::shared_ptr<const Account> held = CacheContainer::Instance()->GetAccount(target);
               if (!held)
                  return refuse(404, _T("There is no account of that address on this server."));
               ownerId = held->GetID();
               name = held->GetAddress();
            }
            else
            {
               std::shared_ptr<const Domain> held = CacheContainer::Instance()->GetDomain(target);
               if (!held)
                  return refuse(404, _T("This server hosts no domain of that name."));
               ownerId = held->GetID();
               name = held->GetName();
            }

            const int scopeNumber = account ? DiscoveryHolds::ScopeAccount : DiscoveryHolds::ScopeDomain;
            for (const DiscoveryStore::Hold &hold : existing)
            {
               if (hold.Live() && hold.scope == scopeNumber && hold.owner_id == ownerId)
                  return refuse(409, Formatter::Format("This case already holds {0}.", name));
            }

            const __int64 id = DiscoveryStore::PlaceHold(theCase, scopeNumber, ownerId, name, by);
            DiscoveryStore::Hold placed;
            if (id <= 0 || !DiscoveryStore::GetHold(id, placed))
               return refuse(500, _T("The hold could not be written."));

            LOG_APPLICATION("RestApi: Discovery case " + StringParser::IntToString(theCase.id) + ": " + name + " placed on hold by " + by + ".");
            return BuildResponse_(201, "{\"placed\":1,\"holds\":[" + HoldJson(placed) + "]}");
         }

         if (scope == "archive")
         {
            DiscoveryStore::Search search;
            if (searchId <= 0 || !DiscoveryStore::GetSearch(searchId, search) || search.case_id != theCase.id)
               return refuse(404, _T("A hold on archived copies names a search of this case in search."));

            if (search.state != DiscoveryStore::JobDone)
               return refuse(409, _T("The search has not finished; its archived copies can be held once it has."));

            std::vector<AnsiString> hits;
            std::vector<AnsiString> unsearched;
            int total = 0;
            int unsearchedListed = 0;
            const String path = FileUtilities::Combine(DiscoveryStore::CaseDirectory(theCase.id), search.result_file);
            if (!DiscoverySearch::ReadResults(path, 0, INT_MAX, hits, total, 0, 0, unsearched, unsearchedListed))
               return refuse(500, _T("The search's results file could not be read: ") + path);

            std::set<__int64> archiveIds;
            for (const AnsiString &line : hits)
            {
               JsonValue hit;
               std::string hitError;
               if (JsonValue::Parse(std::string(line.c_str(), line.GetLength()), hit, hitError) && hit.IsObject() && hit.GetString("kind") == "archive")
                  archiveIds.insert(hit.GetInt64("id"));
            }

            if (archiveIds.size() > MaxArchiveHolds)
               return refuse(409, _T("The search matched more than 10,000 archived copies, which is more than one request puts on hold. Narrow the search, or hold the custodians' accounts and domains, which holds their archived copies too."));

            for (const DiscoveryStore::Hold &hold : existing)
            {
               if (hold.Live() && hold.scope == DiscoveryHolds::ScopeArchive)
                  archiveIds.erase(hold.owner_id);
            }

            AnsiString ids;
            int placed = 0;
            for (const __int64 archiveId : archiveIds)
            {
               PersistentArchiveIndex::Entry entry;
               if (!PersistentArchiveIndex::Get(archiveId, entry))
                  continue;

               if (!DomainIn(caller.domains, entry.domain))
                  continue;

               String name = entry.mailbox.IsEmpty() ? String(_T("inbound")) : entry.mailbox + _T("@") + entry.domain;
               name += _T(" (archive #") + StringParser::IntToString(archiveId) + _T(")");

               const __int64 id = DiscoveryStore::PlaceHold(theCase, DiscoveryHolds::ScopeArchive, archiveId, name, by);
               if (id <= 0)
                  return refuse(500, Formatter::Format("{0} archived copies were put on hold and the next could not be; the holds placed stand.", placed));

               ids += (placed > 0 ? AnsiString(",") : AnsiString()) + Number(id);
               placed++;
            }

            LOG_APPLICATION("RestApi: Discovery case " + StringParser::IntToString(theCase.id) + ": " + StringParser::IntToString(placed) +
                            " archived cop(ies) placed on hold by " + by + ".");
            return BuildResponse_(placed > 0 ? 201 : 200, "{\"placed\":" + Number(placed) + ",\"holds\":[" + ids + "]}");
         }

         return refuse(400, _T("scope must be account, domain or archive."));
      }

      case RouteDiscoveryHoldRelease:
      {
         DiscoveryStore::Hold hold;
         if (!DiscoveryStore::GetHold(route.message_id, hold) || hold.case_id != theCase.id)
            return refuse(404, _T("There is no such hold in this case."));

         if (!hold.Live())
            return refuse(409, _T("The hold was released already."));

         if (!DiscoveryStore::ReleaseHold(theCase, hold, by))
            return refuse(500, _T("The hold could not be released."));

         DiscoveryStore::Hold released;
         DiscoveryStore::GetHold(hold.id, released);

         LOG_APPLICATION("RestApi: Discovery case " + StringParser::IntToString(theCase.id) + ": the hold on " + hold.name + " released by " + by + ".");
         return BuildResponse_(200, HoldJson(released));
      }

      case RouteDiscoveryExportCreate:
      {
         if (!caseOpen)
            return refuse(409, _T("The case is closed: a closed case makes no export. Open a new one."));

         DiscoveryStore::Search search;
         if (!DiscoveryStore::GetSearch(route.message_id, search) || search.case_id != theCase.id)
            return refuse(404, _T("There is no such search in this case."));

         if (search.state != DiscoveryStore::JobDone)
            return refuse(409, Formatter::Format("The search is {0}: only a search that has finished can be exported.", String(StateWord(search.state))));

         const __int64 id = DiscoveryStore::CreateExport(theCase, search, by);
         if (id <= 0)
            return refuse(500, _T("The export could not be written."));

         DiscoverySearch::Kick();

         AnsiString body = "{\"id\":" + Number(id) + ",\"case\":" + Number(theCase.id) + ",\"search\":" + Number(search.id) +
                           ",\"state\":\"queued\",\"status\":\"/api/v1/discovery/cases/" + Number(theCase.id) +
                           "\",\"download\":\"/api/v1/discovery/cases/" + Number(theCase.id) + "/exports/" + Number(id) + "\"}";
         return BuildResponse_(202, body);
      }

      case RouteDiscoveryExportGet:
         return DiscoveryDownload_(caller, route);

      default:
         break;
      }

      return refuse(404, _T("There is no such route."));
   }

   HttpResponse
   RestApiServer::DiscoveryDownload_(const Caller &caller, const Route &route)
   {
      DiscoveryStore::Case theCase;
      DiscoveryStore::Export result;

      if (!DiscoveryStore::GetCase(route.record_id, theCase) || !CaseVisible(caller.domains, AuditActorFor_(caller).name, theCase) ||
          !DiscoveryStore::GetExport(route.message_id, result) || result.case_id != theCase.id)
         return BuildResponse_(404, "{\"error\":\"There is no such export in this case.\"}");

      // An export of a search that read the archive carries archived copies,
      // which a role without view_archive reads nowhere else - beside the
      // view_mailbox SecondPermissionFor_ asks for (docs/Discovery.md). A
      // search that cannot be read is taken to have read it.
      DiscoveryStore::Search exported;
      const bool readsArchive = !DiscoveryStore::GetSearch(result.search_id, exported) || SearchReachesArchive(exported);
      if (readsArchive && !CallerMayReadArchive_(caller))
         return BuildForbiddenResponse_("this export is of a search that read the archive, and taking it needs view_archive as well");

      if (result.state != DiscoveryStore::JobDone)
      {
         AnsiString body = "{\"error\":" + Quoted(result.state == DiscoveryStore::JobFailed
                                                     ? String(_T("The export failed: ")) + result.error
                                                     : String(_T("The export has not finished yet."))) +
                           ",\"state\":\"" + AnsiString(StateWord(result.state)) + "\"}";
         return BuildResponse_(409, body);
      }

      const String path = FileUtilities::Combine(DiscoveryStore::CaseDirectory(theCase.id), result.file);

      unsigned __int64 size = 0;
      if (result.file.IsEmpty() || !FileUtilities::FileSize64(path, size) || !FileUtilities::Exists(path))
         return BuildResponse_(404, "{\"error\":" + Quoted(_T("The export's file is no longer on the server's disk: ") + path) + "}");

      if (size > DownloadCeiling)
      {
         const String sentence = Formatter::Format("The export is larger than 500 MB, which a download does not carry. It is on the server's disk at {0}, and its SHA-256 is {1}.", path, result.sha256);
         return BuildResponse_(413, "{\"error\":" + Quoted(sentence) + "}");
      }

      AnsiString bytes;
      {
         boost::filesystem::path filePath(path.begin(), path.end());
         boost::filesystem::ifstream in(filePath, std::ios::in | std::ios::binary);
         if (in)
         {
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            bytes = content;
         }
      }

      if (bytes.empty())
         return BuildResponse_(500, "{\"error\":\"The export could not be read.\"}");

      // The download writes no row, so it is recorded here: who took the
      // custodians' mail away, and which bytes.
      AuditTrail::Instance()->RecordEvent(_T("discovery export"), DiscoveryStore::CaseLabel(theCase), _T("downloaded"),
         Formatter::Format("export {0} of search {1}, {2} bytes, sha256 {3}", result.id, result.search_id, result.size, result.sha256));

      LOG_APPLICATION("RestApi: Discovery export " + StringParser::IntToString(result.id) + " of case " + StringParser::IntToString(theCase.id) +
                      " downloaded by " + AuditActorFor_(caller).name + ".");

      AnsiString fileName;
      fileName.Format("case-%I64d-export-%I64d.zip", theCase.id, result.id);

      HttpResponse response;
      response.status = 200;
      response.content_type = "application/zip";
      response.body = bytes;
      response.no_compress = true;
      response.extra_headers =
         "Content-Disposition: attachment; filename=\"" + fileName + "\"\r\n"
         "X-Content-Type-Options: nosniff\r\n"
         "Content-Security-Policy: sandbox\r\n"
         "Cache-Control: no-store\r\n";
      return response;
   }

   AnsiString
   RestApiServer::OpenApiDiscoveryPaths_()
   {
      static const char *paths =
         ",\"/api/v1/discovery/cases\":{"
         "\"get\":{\"summary\":\"eDiscovery: the cases\",\"description\":\"{cases:[{id, name, description, state (open or closed), created_by, created, closed_by, closed, search_count, live_holds, export_count}]}, newest first; times in seconds since 1970. Needs view_discovery. A credential confined to named domains is listed the cases whose every search and hold is inside them, and a case with neither only when that credential opened it. docs/Discovery.md.\",\"responses\":{\"200\":{\"description\":\"The cases\"}}},"
         "\"post\":{\"summary\":\"eDiscovery: open a case\",\"description\":\"{name (1 to 255 characters, one line), description (at most 1,000)}. Needs manage_discovery. On the audit trail as a discovery case created. 201 with the case.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"name\"],\"properties\":{\"name\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"}}}}}},\"responses\":{\"201\":{\"description\":\"The case\"},\"400\":{\"description\":\"The body was refused, with the reason\"}}}},"
         "\"/api/v1/discovery/cases/{id}\":{\"get\":{\"summary\":\"eDiscovery: one case, whole\",\"description\":\"The case with its searches (state queued, running, done or failed; query; ran_by; scanned, matched, unsearched and truncated), its holds (scope account, domain or archive; name; placed and released, and by whom; live) and its exports (state, file, size, sha256 of the zip, items written, withheld, download). A case a confined credential may not see is the same 404 as none.\",\"responses\":{\"200\":{\"description\":\"The case\"},\"404\":{\"description\":\"No such case\"}}}},"
         "\"/api/v1/discovery/cases/{id}/close\":{\"post\":{\"summary\":\"eDiscovery: close a case\",\"description\":\"Closes the case and releases every hold of it still live, each release on the audit trail; the answer carries released, how many. A closed case runs no search, places no hold and makes no export. Needs manage_discovery.\",\"responses\":{\"200\":{\"description\":\"The case, closed\"},\"409\":{\"description\":\"Closed already\"}}}},"
         "\"/api/v1/discovery/cases/{id}/searches\":{\"post\":{\"summary\":\"eDiscovery: run a search\",\"description\":\"{custodians: [addresses, domains, or * for every mailbox and the whole archive - at most 100], from and to (YYYY-MM-DD, inclusive, the message's Date field or, lacking one, the day it was stored), words (every one must be found; a quoted phrase is one; case-insensitive, as IMAP SEARCH TEXT reads: the decoded header, the text and HTML bodies and the text of text/* attachments), attachment ({name contains, type - a media type or a family ending in / - and min_kb}, each optional), scope (mailboxes, archive or both, the default)}. Queued on the maintenance work queue, one search or export at a time; 202 with the search's id. It reads the custodians' folders, their deleted items and their archived copies, each message whole up to 80 MB - a message it cannot read is listed as unsearched with the reason - a zero-access mailbox and a message encrypted end to end on their headers alone, and writes at most 100,000 hits. The whole query is on the audit trail while the audit trail is on, and in the search's own row whatever it says, so one longer than 1,800 characters is refused. A confined credential names no custodian outside its domains, and never *. Needs manage_discovery, and view_archive as well for scope archive or both.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"custodians\"],\"properties\":{\"custodians\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"from\":{\"type\":\"string\"},\"to\":{\"type\":\"string\"},\"words\":{\"type\":\"string\"},\"attachment\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"},\"min_kb\":{\"type\":\"integer\"}}},\"scope\":{\"type\":\"string\"}}}}}},\"responses\":{\"202\":{\"description\":\"Queued\"},\"400\":{\"description\":\"The query was refused, with the reason\"},\"403\":{\"description\":\"A custodian outside the credential's domains, or the archive in scope for a role without view_archive\"},\"404\":{\"description\":\"No such case\"},\"409\":{\"description\":\"The case is closed\"}}}},"
         "\"/api/v1/discovery/cases/{id}/searches/{search}\":{\"get\":{\"summary\":\"eDiscovery: a search and a page of its hits\",\"description\":\"The search's state and counts, and - once it is done - hits from offset (default 0), at most limit (default 100, at most 1000), each {n, kind (mailbox, recoverable or archive), custodian, account_id, id, folder, message_id, date, from, to, subject, size, attachments (names, or null when the body was not read), headers_only (only its headers could be read: a zero-access mailbox, or a message encrypted end to end), zero_access (from a zero-access mailbox, which an export withholds), held}; total; and, paged on their own from unsearched_offset (default 0), at most unsearched_limit (default 100, at most 1000), unsearched_list, the messages it could not search with the reason, of the unsearched_listed the results file lists (at most 10,000; the search's unsearched counts every one). Needs view_discovery, and view_archive as well for a search that read the archive.\",\"parameters\":[{\"name\":\"offset\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"}},{\"name\":\"limit\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"}},{\"name\":\"unsearched_offset\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"}},{\"name\":\"unsearched_limit\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"}}],\"responses\":{\"200\":{\"description\":\"The search\"},\"403\":{\"description\":\"The search read the archive and the role does not hold view_archive\"},\"404\":{\"description\":\"No such search in the case\"},\"500\":{\"description\":\"The search's results file could not be read\"}}}},"
         "\"/api/v1/discovery/cases/{id}/searches/{search}/export\":{\"post\":{\"summary\":\"eDiscovery: export a search's hits\",\"description\":\"Queued as a search is; the zip is written to the case's directory under the data directory: every hit as its own bytes under <custodian>/<folder>/<n>-<message id>.eml, an mbox per custodian folder (in parts of at most 256 MB), SHA256SUMS, and manifest.json naming the case, the search and its query, every item with its SHA-256, size, message id, date, from, to, subject and hold state, every hit withheld with the reason - a zero-access mailbox's messages are withheld by design, a mailbox that has zero access on when the export runs included - what the search could not read (unsearched_list), and the SHA-256 of SHA256SUMS. The zip's own SHA-256 is on the case. An export that would pass 4 GB fails with the sentence, since the zip format this server writes is the 32-bit one, and so does one that would leave the data directory's volume less than a gigabyte free. Nothing removes an export. No PST. Needs manage_discovery.\",\"responses\":{\"202\":{\"description\":\"Queued\"},\"404\":{\"description\":\"No such search in the case\"},\"409\":{\"description\":\"The search has not finished, or the case is closed\"}}}},"
         "\"/api/v1/discovery/cases/{id}/holds\":{\"post\":{\"summary\":\"eDiscovery: place a hold\",\"description\":\"{scope: account, target: an address} or {scope: domain, target: a domain} holds every message of the account, or of every account of the domain, and its archived copies: a delete by the person, an account's own rule, Sieve script, forward and spam delete threshold, retention, the recover-deleted sweep, a restore's replace, a full restore, a move to another domain and the account's or domain's deletion keep the message or are refused - docs/Discovery.md lists every path, and what a hold does not reach. {scope: archive, search: a search of this case} holds the archived copies that search matched, at most 10,000 at once. Needs manage_discovery; a confined credential holds nothing outside its domains.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"scope\"],\"properties\":{\"scope\":{\"type\":\"string\"},\"target\":{\"type\":\"string\"},\"search\":{\"type\":\"integer\"}}}}}},\"responses\":{\"200\":{\"description\":\"placed 0: every archived copy the search matched is held by this case already\"},\"201\":{\"description\":\"The holds placed\"},\"400\":{\"description\":\"The body, a field of it or the scope was refused, with the reason\"},\"403\":{\"description\":\"An account or domain outside the credential's domains\"},\"404\":{\"description\":\"No such case, account, domain or search\"},\"409\":{\"description\":\"Held by this case already, the search unfinished, more than 10,000 archived copies matched, or the case closed\"},\"500\":{\"description\":\"The hold, or one of the archived copies' holds, could not be written; those placed stand\"}}}},"
         "\"/api/v1/discovery/cases/{id}/holds/{hold}\":{\"delete\":{\"summary\":\"eDiscovery: release a hold\",\"description\":\"The hold is released, on the audit trail; what it kept is kept by the case's other holds, or ends at the next sweep. Needs manage_discovery.\",\"responses\":{\"200\":{\"description\":\"The hold, released\"},\"409\":{\"description\":\"Released already\"}}}},"
         "\"/api/v1/discovery/cases/{id}/exports/{export}\":{\"get\":{\"summary\":\"eDiscovery: download an export\",\"description\":\"The zip, as application/zip with Content-Disposition, nosniff, sandbox and no-store; read into memory once, so one past 500 MB is 413 with where it is on the server's disk. The download is on the audit trail as a discovery export downloaded, with the zip's SHA-256, while the audit trail is on. Needs view_discovery and view_mailbox: it hands over the mail itself, the custodians' deleted items included; and view_archive as well for an export of a search that read the archive.\",\"responses\":{\"200\":{\"description\":\"The zip\"},\"403\":{\"description\":\"The role lacks view_mailbox, or view_archive for an export of a search that read the archive\"},\"404\":{\"description\":\"No such export in the case, or its file is no longer on the server's disk\"},\"409\":{\"description\":\"The export has not finished, or failed\"},\"413\":{\"description\":\"Past 500 MB\"}}}}";
      return AnsiString(paths);
   }
}
