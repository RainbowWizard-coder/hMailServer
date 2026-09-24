// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See DiscoveryStore.h.

#include "StdAfx.h"
#include "DiscoveryStore.h"
#include "DiscoveryHolds.h"
#include "AuditTrail.h"
#include "FileUtilities.h"
#include "Unicode.h"

#include "../Application/Application.h"
#include "../Application/IniFileSettings.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"

#include <ctime>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      __int64 Now()
      {
         return (__int64) std::time(nullptr);
      }

      String Clip(const String &value, int length)
      {
         return value.GetLength() > length ? value.Mid(0, length) : value;
      }

      const wchar_t *const CasesTable = L"hm_discoverycases";
      const wchar_t *const SearchesTable = L"hm_discoverysearches";
      const wchar_t *const HoldsTable = L"hm_discoveryholds";
      const wchar_t *const ExportsTable = L"hm_discoveryexports";
   }

   String
   DiscoveryStore::CaseLabel(const Case &theCase)
   {
      String label;
      label.Format(_T("case %I64d: %s"), theCase.id, theCase.name.c_str());
      return label;
   }

   String
   DiscoveryStore::CaseDirectory(__int64 caseId)
   {
      String name;
      name.Format(_T("case-%I64d"), caseId);
      return FileUtilities::Combine(FileUtilities::Combine(IniFileSettings::Instance()->GetDataDirectory(), _T("Discovery")), name);
   }

   // ---------------------------------------------------------------- cases

   void
   DiscoveryStore::ReadCase_(std::shared_ptr<DALRecordset> recordset, Case &theCase)
   {
      theCase.id = recordset->GetInt64Value("caseid");
      theCase.name = recordset->GetStringValue("casename");
      theCase.description = recordset->GetStringValue("casedescription");
      theCase.state = (int) recordset->GetLongValue("casestate");
      theCase.created_by = recordset->GetStringValue("casecreatedby");
      theCase.created = recordset->GetInt64Value("casecreated");
      theCase.closed_by = recordset->GetStringValue("caseclosedby");
      theCase.closed = recordset->GetInt64Value("caseclosed");
   }

   bool
   DiscoveryStore::ListCases(std::vector<Case> &cases)
   {
      cases.clear();

      SQLCommand command(_T("select caseid, casename, casedescription, casestate, casecreatedby, casecreated, caseclosedby, caseclosed from hm_discoverycases order by caseid desc"));
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         Case theCase;
         ReadCase_(recordset, theCase);
         cases.push_back(theCase);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   DiscoveryStore::GetCase(__int64 id, Case &found)
   {
      SQLCommand command(_T("select caseid, casename, casedescription, casestate, casecreatedby, casecreated, caseclosedby, caseclosed from hm_discoverycases where caseid = @CASEID"));
      command.AddParameter("@CASEID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadCase_(recordset, found);
      return true;
   }

   __int64
   DiscoveryStore::CreateCase(const String &name, const String &description, const String &createdBy)
   {
      // Named on the audit trail by its name column, as a domain is; it has no
      // id to be named by until the insert has made one.
      SQLStatement statement(SQLStatement::STInsert, CasesTable);
      statement.AddColumn("casename", Clip(name, 255));
      statement.AddColumn("casedescription", Clip(description, 1000));
      statement.AddColumn("casestate", (long) CaseOpen);
      statement.AddColumn("casecreatedby", Clip(createdBy, 255));
      statement.AddColumnInt64("casecreated", Now());
      statement.AddColumn("caseclosedby", String());
      statement.AddColumnInt64("caseclosed", 0);
      statement.SetIdentityColumn("caseid");

      __int64 id = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &id))
         return 0;

      return id;
   }

   bool
   DiscoveryStore::CloseCase(const Case &theCase, const String &closedBy, int &released)
   {
      released = 0;

      {
         AuditDescription words(CasesTable, CaseLabel(theCase));

         SQLCommand command(_T("update hm_discoverycases set casestate = @STATE, caseclosedby = @CLOSEDBY, caseclosed = @CLOSED where caseid = @CASEID"));
         command.AddParameter("@STATE", (int) CaseClosed);
         command.AddParameter("@CLOSEDBY", Clip(closedBy, 255));
         command.AddParameter("@CLOSED", Now());
         command.AddParameter("@CASEID", theCase.id);

         if (!Application::Instance()->GetDBManager()->Execute(command))
            return false;
      }

      // A closed case holds nothing: every hold it placed and has not released
      // is released now, each on the trail as a release is.
      std::vector<Hold> holds;
      if (ListHolds(theCase.id, holds))
      {
         for (const Hold &hold : holds)
         {
            if (hold.Live() && ReleaseHold(theCase, hold, closedBy))
               released++;
         }
      }

      return true;
   }

   // ---------------------------------------------------------------- searches

   void
   DiscoveryStore::ReadSearch_(std::shared_ptr<DALRecordset> recordset, Search &search)
   {
      search.id = recordset->GetInt64Value("searchid");
      search.case_id = recordset->GetInt64Value("searchcaseid");
      Unicode::WideToMultiByte(recordset->GetStringValue("searchquery"), search.query);
      search.ran_by = recordset->GetStringValue("searchranby");
      search.ran = recordset->GetInt64Value("searchran");
      search.finished = recordset->GetInt64Value("searchfinished");
      search.state = (int) recordset->GetLongValue("searchstate");
      search.scanned = (int) recordset->GetLongValue("searchscanned");
      search.matched = (int) recordset->GetLongValue("searchmatched");
      search.unsearched = (int) recordset->GetLongValue("searchunsearched");
      search.truncated = recordset->GetLongValue("searchtruncated") != 0;
      search.error = recordset->GetStringValue("searcherror");
      search.result_file = recordset->GetStringValue("searchresultfile");
   }

   bool
   DiscoveryStore::ListSearches(__int64 caseId, std::vector<Search> &searches)
   {
      searches.clear();

      SQLCommand command(_T("select searchid, searchcaseid, searchquery, searchranby, searchran, searchfinished, searchstate, searchscanned, searchmatched, searchunsearched, searchtruncated, searcherror, searchresultfile from hm_discoverysearches where searchcaseid = @CASEID order by searchid"));
      command.AddParameter("@CASEID", caseId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         Search search;
         ReadSearch_(recordset, search);
         searches.push_back(search);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   DiscoveryStore::GetSearch(__int64 id, Search &found)
   {
      SQLCommand command(_T("select searchid, searchcaseid, searchquery, searchranby, searchran, searchfinished, searchstate, searchscanned, searchmatched, searchunsearched, searchtruncated, searcherror, searchresultfile from hm_discoverysearches where searchid = @SEARCHID"));
      command.AddParameter("@SEARCHID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadSearch_(recordset, found);
      return true;
   }

   __int64
   DiscoveryStore::CreateSearch(const Case &theCase, const AnsiString &queryJson, const String &ranBy)
   {
      String query;
      Unicode::MultiByteToWide(queryJson, query);

      // The whole query on the trail, which the route has kept short enough to
      // go there whole: "every search logged" is this row.
      const String detail = String(_T("query=")) + query + String(_T(", ranby=")) + ranBy;
      AuditDescription words(SearchesTable, CaseLabel(theCase), detail);

      SQLStatement statement(SQLStatement::STInsert, SearchesTable);
      statement.AddColumnInt64("searchcaseid", theCase.id);
      statement.AddColumn("searchquery", query);
      statement.AddColumn("searchranby", Clip(ranBy, 255));
      statement.AddColumnInt64("searchran", Now());
      statement.AddColumnInt64("searchfinished", 0);
      statement.AddColumn("searchstate", (long) JobQueued);
      statement.AddColumn("searchscanned", (long) 0);
      statement.AddColumn("searchmatched", (long) 0);
      statement.AddColumn("searchunsearched", (long) 0);
      statement.AddColumn("searchtruncated", (long) 0);
      statement.AddColumn("searcherror", String());
      statement.AddColumn("searchresultfile", String());
      statement.SetIdentityColumn("searchid");

      __int64 id = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &id))
         return 0;

      return id;
   }

   bool
   DiscoveryStore::MarkSearchRunning(__int64 id)
   {
      SQLCommand command(_T("update hm_discoverysearches set searchstate = @STATE where searchid = @SEARCHID"));
      command.AddParameter("@STATE", (int) JobRunning);
      command.AddParameter("@SEARCHID", id);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   DiscoveryStore::RecordSearchProgress(__int64 id, int scanned, int matched, int unsearched)
   {
      SQLCommand command(_T("update hm_discoverysearches set searchscanned = @SCANNED, searchmatched = @MATCHED, searchunsearched = @UNSEARCHED where searchid = @SEARCHID"));
      command.AddParameter("@SCANNED", scanned);
      command.AddParameter("@MATCHED", matched);
      command.AddParameter("@UNSEARCHED", unsearched);
      command.AddParameter("@SEARCHID", id);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   DiscoveryStore::FinishSearch(const Search &search)
   {
      SQLCommand command(_T("update hm_discoverysearches set searchstate = @STATE, searchfinished = @FINISHED, searchscanned = @SCANNED, searchmatched = @MATCHED, searchunsearched = @UNSEARCHED, searchtruncated = @TRUNCATED, searcherror = @ERROR, searchresultfile = @RESULTFILE where searchid = @SEARCHID"));
      command.AddParameter("@STATE", search.state);
      command.AddParameter("@FINISHED", Now());
      command.AddParameter("@SCANNED", search.scanned);
      command.AddParameter("@MATCHED", search.matched);
      command.AddParameter("@UNSEARCHED", search.unsearched);
      command.AddParameter("@TRUNCATED", search.truncated ? 1 : 0);
      command.AddParameter("@ERROR", Clip(search.error, 1000));
      command.AddParameter("@RESULTFILE", Clip(search.result_file, 255));
      command.AddParameter("@SEARCHID", search.id);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   // ---------------------------------------------------------------- holds

   void
   DiscoveryStore::ReadHold_(std::shared_ptr<DALRecordset> recordset, Hold &hold)
   {
      hold.id = recordset->GetInt64Value("holdid");
      hold.case_id = recordset->GetInt64Value("holdcaseid");
      hold.scope = (int) recordset->GetLongValue("holdscope");
      hold.owner_id = recordset->GetInt64Value("holdownerid");
      hold.name = recordset->GetStringValue("holdname");
      hold.placed_by = recordset->GetStringValue("holdplacedby");
      hold.placed = recordset->GetInt64Value("holdplaced");
      hold.released_by = recordset->GetStringValue("holdreleasedby");
      hold.released = recordset->GetInt64Value("holdreleased");
   }

   bool
   DiscoveryStore::ListHolds(__int64 caseId, std::vector<Hold> &holds)
   {
      holds.clear();

      SQLCommand command(_T("select holdid, holdcaseid, holdscope, holdownerid, holdname, holdplacedby, holdplaced, holdreleasedby, holdreleased from hm_discoveryholds where holdcaseid = @CASEID order by holdid"));
      command.AddParameter("@CASEID", caseId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         Hold hold;
         ReadHold_(recordset, hold);
         holds.push_back(hold);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   DiscoveryStore::GetHold(__int64 id, Hold &found)
   {
      SQLCommand command(_T("select holdid, holdcaseid, holdscope, holdownerid, holdname, holdplacedby, holdplaced, holdreleasedby, holdreleased from hm_discoveryholds where holdid = @HOLDID"));
      command.AddParameter("@HOLDID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadHold_(recordset, found);
      return true;
   }

   __int64
   DiscoveryStore::PlaceHold(const Case &theCase, int scope, __int64 ownerId, const String &name, const String &placedBy)
   {
      __int64 id = 0;

      {
         // Named by the case; the detail is the row's own columns - the scope,
         // the owner's id and what it was called, who placed it and when.
         AuditDescription words(HoldsTable, CaseLabel(theCase));

         SQLStatement statement(SQLStatement::STInsert, HoldsTable);
         statement.AddColumnInt64("holdcaseid", theCase.id);
         statement.AddColumn("holdscope", (long) scope);
         statement.AddColumnInt64("holdownerid", ownerId);
         statement.AddColumn("holdname", Clip(name, 255));
         statement.AddColumn("holdplacedby", Clip(placedBy, 255));
         statement.AddColumnInt64("holdplaced", Now());
         statement.AddColumn("holdreleasedby", String());
         statement.AddColumnInt64("holdreleased", 0);
         statement.SetIdentityColumn("holdid");

         if (!Application::Instance()->GetDBManager()->Execute(statement, &id))
            id = 0;
      }

      // Whatever the outcome: a hold that was written must be honoured by the
      // very next delete, and one that was not changes nothing by being read.
      DiscoveryHolds::Invalidate();
      return id;
   }

   bool
   DiscoveryStore::ReleaseHold(const Case &theCase, const Hold &hold, const String &releasedBy)
   {
      bool released = false;

      {
         AuditDescription words(HoldsTable, CaseLabel(theCase));

         SQLCommand command(_T("update hm_discoveryholds set holdreleasedby = @RELEASEDBY, holdreleased = @RELEASED where holdid = @HOLDID and holdreleased = 0"));
         command.AddParameter("@RELEASEDBY", Clip(releasedBy, 255));
         command.AddParameter("@RELEASED", Now());
         command.AddParameter("@HOLDID", hold.id);

         released = Application::Instance()->GetDBManager()->Execute(command);
      }

      DiscoveryHolds::Invalidate();
      return released;
   }

   // ---------------------------------------------------------------- exports

   void
   DiscoveryStore::ReadExport_(std::shared_ptr<DALRecordset> recordset, Export &result)
   {
      result.id = recordset->GetInt64Value("exportid");
      result.case_id = recordset->GetInt64Value("exportcaseid");
      result.search_id = recordset->GetInt64Value("exportsearchid");
      result.state = (int) recordset->GetLongValue("exportstate");
      result.file = recordset->GetStringValue("exportfile");
      result.size = recordset->GetInt64Value("exportsize");
      result.sha256 = recordset->GetStringValue("exportsha256");
      result.items = (int) recordset->GetLongValue("exportitems");
      result.withheld = (int) recordset->GetLongValue("exportwithheld");
      result.requested_by = recordset->GetStringValue("exportrequestedby");
      result.requested = recordset->GetInt64Value("exportrequested");
      result.finished = recordset->GetInt64Value("exportfinished");
      result.error = recordset->GetStringValue("exporterror");
   }

   bool
   DiscoveryStore::ListExports(__int64 caseId, std::vector<Export> &exports)
   {
      exports.clear();

      SQLCommand command(_T("select exportid, exportcaseid, exportsearchid, exportstate, exportfile, exportsize, exportsha256, exportitems, exportwithheld, exportrequestedby, exportrequested, exportfinished, exporterror from hm_discoveryexports where exportcaseid = @CASEID order by exportid"));
      command.AddParameter("@CASEID", caseId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         Export result;
         ReadExport_(recordset, result);
         exports.push_back(result);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   DiscoveryStore::GetExport(__int64 id, Export &found)
   {
      SQLCommand command(_T("select exportid, exportcaseid, exportsearchid, exportstate, exportfile, exportsize, exportsha256, exportitems, exportwithheld, exportrequestedby, exportrequested, exportfinished, exporterror from hm_discoveryexports where exportid = @EXPORTID"));
      command.AddParameter("@EXPORTID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadExport_(recordset, found);
      return true;
   }

   __int64
   DiscoveryStore::CreateExport(const Case &theCase, const Search &search, const String &requestedBy)
   {
      AuditDescription words(ExportsTable, CaseLabel(theCase));

      SQLStatement statement(SQLStatement::STInsert, ExportsTable);
      statement.AddColumnInt64("exportcaseid", theCase.id);
      statement.AddColumnInt64("exportsearchid", search.id);
      statement.AddColumn("exportstate", (long) JobQueued);
      statement.AddColumn("exportfile", String());
      statement.AddColumnInt64("exportsize", 0);
      statement.AddColumn("exportsha256", String());
      statement.AddColumn("exportitems", (long) 0);
      statement.AddColumn("exportwithheld", (long) 0);
      statement.AddColumn("exportrequestedby", Clip(requestedBy, 255));
      statement.AddColumnInt64("exportrequested", Now());
      statement.AddColumnInt64("exportfinished", 0);
      statement.AddColumn("exporterror", String());
      statement.SetIdentityColumn("exportid");

      __int64 id = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &id))
         return 0;

      return id;
   }

   bool
   DiscoveryStore::MarkExportRunning(__int64 id)
   {
      SQLCommand command(_T("update hm_discoveryexports set exportstate = @STATE where exportid = @EXPORTID"));
      command.AddParameter("@STATE", (int) JobRunning);
      command.AddParameter("@EXPORTID", id);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   DiscoveryStore::FinishExport(const Export &result)
   {
      SQLCommand command(_T("update hm_discoveryexports set exportstate = @STATE, exportfile = @FILE, exportsize = @SIZE, exportsha256 = @SHA, exportitems = @ITEMS, exportwithheld = @WITHHELD, exportfinished = @FINISHED, exporterror = @ERROR where exportid = @EXPORTID"));
      command.AddParameter("@STATE", result.state);
      command.AddParameter("@FILE", Clip(result.file, 255));
      command.AddParameter("@SIZE", result.size);
      command.AddParameter("@SHA", Clip(result.sha256, 64));
      command.AddParameter("@ITEMS", result.items);
      command.AddParameter("@WITHHELD", result.withheld);
      command.AddParameter("@FINISHED", Now());
      command.AddParameter("@ERROR", Clip(result.error, 1000));
      command.AddParameter("@EXPORTID", result.id);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   // ---------------------------------------------------------------- the queue

   bool
   DiscoveryStore::OldestQueued(bool &isSearch, __int64 &id)
   {
      isSearch = false;
      id = 0;

      __int64 searchId = 0, searchAsked = 0, exportId = 0, exportAsked = 0;

      SQLCommand searches(_T("select searchid, searchran from hm_discoverysearches where searchstate = @STATE order by searchid"));
      searches.AddParameter("@STATE", (int) JobQueued);
      std::shared_ptr<DALRecordset> searchRows = Application::Instance()->GetDBManager()->OpenRecordset(searches);
      if (!searchRows)
         return false;

      if (!searchRows->IsEOF())
      {
         searchId = searchRows->GetInt64Value("searchid");
         searchAsked = searchRows->GetInt64Value("searchran");
      }

      SQLCommand exports(_T("select exportid, exportrequested from hm_discoveryexports where exportstate = @STATE order by exportid"));
      exports.AddParameter("@STATE", (int) JobQueued);
      std::shared_ptr<DALRecordset> exportRows = Application::Instance()->GetDBManager()->OpenRecordset(exports);
      if (!exportRows)
         return false;

      if (!exportRows->IsEOF())
      {
         exportId = exportRows->GetInt64Value("exportid");
         exportAsked = exportRows->GetInt64Value("exportrequested");
      }

      if (searchId == 0 && exportId == 0)
         return false;

      // An export waits for nothing but its turn - its search was done when it
      // was asked for - so the two are taken in the order they were asked.
      if (exportId == 0 || (searchId != 0 && searchAsked <= exportAsked))
      {
         isSearch = true;
         id = searchId;
      }
      else
      {
         isSearch = false;
         id = exportId;
      }

      return true;
   }

   void
   DiscoveryStore::FailInterrupted()
   {
      const String reason = _T("The server stopped while this ran, so it did not finish. Ask for it again.");

      SQLCommand searches(_T("update hm_discoverysearches set searchstate = @FAILED, searcherror = @ERROR, searchfinished = @FINISHED where searchstate = @RUNNING"));
      searches.AddParameter("@FAILED", (int) JobFailed);
      searches.AddParameter("@ERROR", reason);
      searches.AddParameter("@FINISHED", Now());
      searches.AddParameter("@RUNNING", (int) JobRunning);
      Application::Instance()->GetDBManager()->Execute(searches);

      SQLCommand exports(_T("update hm_discoveryexports set exportstate = @FAILED, exporterror = @ERROR, exportfinished = @FINISHED where exportstate = @RUNNING"));
      exports.AddParameter("@FAILED", (int) JobFailed);
      exports.AddParameter("@ERROR", reason);
      exports.AddParameter("@FINISHED", Now());
      exports.AddParameter("@RUNNING", (int) JobRunning);
      Application::Instance()->GetDBManager()->Execute(exports);
   }
}
