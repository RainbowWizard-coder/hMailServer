// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "PersistentDkimRotation.h"
#include "PersistentDomain.h"

#include "../BO/Domain.h"
#include "../Util/GUIDCreator.h"
#include "../SQL/DALConnection.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"

#include <algorithm>
#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      typedef DkimRotationPlan::State State;
      typedef DkimRotationPlan::Slot Slot;

      // The columns are 255 characters. A selector is at most 63 and a key file path
      // is the domain's own column's width, so nothing is lost there; a detail is
      // what a check found, already shortened.
      String Fit(const String &value)
      {
         return value.GetLength() > 255 ? value.Mid(0, 255) : value;
      }

      void ReadState(const std::shared_ptr<DALRecordset> &recordset, State &state)
      {
         auto slot = [&recordset](const char *prefix, Slot &slot, bool hasSince, const char *sinceColumn)
         {
            AnsiString base = AnsiString("dkr") + prefix;
            slot.selector = AnsiString(recordset->GetStringValue(base + "selector"));
            slot.key_file = recordset->GetStringValue(base + "keyfile");
            slot.generated = recordset->GetLongValue(base + "generated") == 1;
            slot.status = (int) recordset->GetLongValue(base + "status");
            slot.checked = recordset->GetInt64Value(base + "checked");
            slot.detail = recordset->GetStringValue(base + "detail");
            if (hasSince)
               slot.since = recordset->GetInt64Value(base + sinceColumn);
         };

         slot("current", state.current, true, "since");
         slot("next", state.next, true, "created");
         slot("retiring", state.retiring, true, "since");

         state.next_first_seen = recordset->GetInt64Value("dkrnextfirstseen");
         state.retired_selector = AnsiString(recordset->GetStringValue("dkrretiredselector"));
         state.retired_at = recordset->GetInt64Value("dkrretiredat");
         state.retired_key_file = recordset->GetStringValue("dkrretiredkeyfile");
         state.last_error = recordset->GetStringValue("dkrlasterror");
         state.last_error_time = recordset->GetInt64Value("dkrlasterrortime");
      }

      void AddColumns(SQLStatement &statement, const State &state)
      {
         auto slot = [&statement](const char *prefix, const Slot &slot, const char *sinceColumn)
         {
            String base = String(_T("dkr")) + String(prefix);
            statement.AddColumn(base + _T("selector"), Fit(String(slot.selector)));
            statement.AddColumn(base + _T("keyfile"), Fit(slot.key_file));
            statement.AddColumn(base + _T("generated"), (long) (slot.generated ? 1 : 0));
            statement.AddColumnInt64(base + String(sinceColumn), slot.since);
            statement.AddColumn(base + _T("status"), (long) slot.status);
            statement.AddColumnInt64(base + _T("checked"), slot.checked);
            statement.AddColumn(base + _T("detail"), Fit(slot.detail));
         };

         slot("current", state.current, "since");
         slot("next", state.next, "created");
         slot("retiring", state.retiring, "since");

         statement.AddColumnInt64(_T("dkrnextfirstseen"), state.next_first_seen);
         statement.AddColumn(_T("dkrretiredselector"), Fit(String(state.retired_selector)));
         statement.AddColumnInt64(_T("dkrretiredat"), state.retired_at);
         statement.AddColumn(_T("dkrretiredkeyfile"), Fit(state.retired_key_file));
         statement.AddColumn(_T("dkrlasterror"), Fit(state.last_error));
         statement.AddColumnInt64(_T("dkrlasterrortime"), state.last_error_time);
      }

      // -1 when the query failed, 0 when there is no row, else the row id.
      __int64 FindRow(__int64 domainId)
      {
         SQLCommand command("select dkrid from hm_dkimrotations where dkrdomainid = @DOMAINID");
         command.AddParameter("@DOMAINID", domainId);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return -1;

         if (recordset->IsEOF())
            return 0;

         return recordset->GetInt64Value("dkrid");
      }

      bool Update(__int64 rowId, __int64 domainId, const State &state)
      {
         SQLStatement statement;
         statement.SetStatementType(SQLStatement::STUpdate);
         statement.SetTable("hm_dkimrotations");
         AddColumns(statement, state);

         String where;
         where.Format(_T("dkrid = %I64d and dkrdomainid = %I64d"), rowId, domainId);
         statement.SetWhereClause(where);

         return Application::Instance()->GetDBManager()->Execute(statement);
      }
   }

   bool
   PersistentDkimRotation::Read(__int64 domainId, State &state, bool &exists)
   {
      exists = false;

      SQLCommand command("select * from hm_dkimrotations where dkrdomainid = @DOMAINID");
      command.AddParameter("@DOMAINID", domainId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      if (recordset->IsEOF())
         return true;

      ReadState(recordset, state);
      exists = true;
      return true;
   }

   bool
   PersistentDkimRotation::Save(__int64 domainId, const State &state)
   {
      __int64 rowId = FindRow(domainId);
      if (rowId < 0)
         return false;

      if (rowId > 0)
         return Update(rowId, domainId, state);

      return InsertOrUpdate_(domainId, state);
   }

   bool
   PersistentDkimRotation::InsertOrUpdate_(__int64 domainId, const State &state)
   {
      SQLStatement statement;
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetTable("hm_dkimrotations");
      statement.SetIdentityColumn("dkrid");
      statement.AddColumnInt64("dkrdomainid", domainId);
      AddColumns(statement, state);

      // A collision on the unique index is not a fault: the engine's mutex keeps
      // two passes in this process apart, and a second node sharing the database
      // is the case left - its row is then updated below rather than reported.
      // The collision is ignored (DALErrorInSQL), and every backend answers an
      // ignored error with success and no id, so a row written is told from one
      // refused by its id. Until 6.3.6 the success alone was taken for a write,
      // the update below never ran, and this pass's state was lost to the other
      // node's.
      __int64 newId = 0;
      if (Application::Instance()->GetDBManager()->Execute(statement, &newId, DALConnection::DALErrorInSQL) && newId > 0)
         return true;

      const __int64 rowId = FindRow(domainId);
      return rowId > 0 && Update(rowId, domainId, state);
   }

   bool
   PersistentDkimRotation::ListInFlight(std::vector<__int64> &domainIds)
   {
      domainIds.clear();

      SQLCommand scheduled("select domainid from hm_domains where domaindkimrotationenabled = 1");
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(scheduled);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         domainIds.push_back(recordset->GetInt64Value("domainid"));
         recordset->MoveNext();
      }

      SQLCommand inFlight("select dkrdomainid, dkrnextselector, dkrretiringselector from hm_dkimrotations");
      recordset = Application::Instance()->GetDBManager()->OpenRecordset(inFlight);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         // Filtered here rather than with "<> ''": an empty string and NULL are the
         // same thing to one of the four backends and not to the others, and a
         // comparison that disagrees across them is the last thing this wants.
         if (!recordset->GetStringValue("dkrnextselector").IsEmpty() ||
             !recordset->GetStringValue("dkrretiringselector").IsEmpty())
         {
            __int64 id = recordset->GetInt64Value("dkrdomainid");
            if (std::find(domainIds.begin(), domainIds.end(), id) == domainIds.end())
               domainIds.push_back(id);
         }

         recordset->MoveNext();
      }

      return true;
   }

   bool
   PersistentDkimRotation::ListRetiringKeyFiles(std::vector<std::pair<__int64, String> > &files)
   {
      files.clear();

      SQLCommand command("select dkrdomainid, dkrretiringkeyfile from hm_dkimrotations");
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         String file = recordset->GetStringValue("dkrretiringkeyfile");
         if (!file.IsEmpty())
            files.push_back(std::make_pair(recordset->GetInt64Value("dkrdomainid"), file));
         recordset->MoveNext();
      }

      return true;
   }

   bool
   PersistentDkimRotation::ListAll(std::vector<std::pair<__int64, State> > &rows)
   {
      rows.clear();

      SQLCommand command("select * from hm_dkimrotations");
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         State state;
         ReadState(recordset, state);
         rows.push_back(std::make_pair(recordset->GetInt64Value("dkrdomainid"), state));
         recordset->MoveNext();
      }

      return true;
   }

   namespace
   {
      void RotationExpect(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(std::string("PersistentDkimRotationTester: ") + what);
      }

      __int64 RotationRows(__int64 domainId)
      {
         SQLCommand command("select count(*) as rows_ from hm_dkimrotations where dkrdomainid = @DOMAINID");
         command.AddParameter("@DOMAINID", domainId);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset || recordset->IsEOF())
            return -1;
         return recordset->GetInt64Value("rows_");
      }

      void DeleteRotationRows(__int64 domainId)
      {
         SQLCommand command("delete from hm_dkimrotations where dkrdomainid = @DOMAINID");
         command.AddParameter("@DOMAINID", domainId);
         Application::Instance()->GetDBManager()->Execute(command);
      }

      State NamedState(const char *selector, __int64 since)
      {
         State state;
         state.current.selector = selector;
         state.current.key_file = _T("C:\\rotation-tester\\") + String(selector) + _T(".pem");
         state.current.since = since;
         state.last_error = String(selector);
         state.last_error_time = since;
         return state;
      }
   }

   void
   PersistentDkimRotationTester::Test()
   {
      // A domain of the tester's own, so that no row of the bench's is touched;
      // the row cascades from it, and both go at the end whatever happened.
      String name = GUIDCreator::GetGUID();
      name.Remove(L'{');
      name.Remove(L'}');
      name.Remove(L'-');
      name = _T("rotation-") + name.Mid(0, 12) + _T(".tester.example");
      name.MakeLower();

      std::shared_ptr<Domain> domain(new Domain());
      domain->SetName(name);
      domain->SetIsActive(false);

      String saveError;
      RotationExpect(PersistentDomain::SaveObject(domain, saveError, PersistenceModeNormal) && domain->GetID() > 0,
         "the tester's domain could not be made");
      const __int64 domainId = domain->GetID();

      try
      {
         const State theirs = NamedState("theirs", 1000);
         const State ours = NamedState("ours", 2000);

         // The ordinary first write: no row, so the insert, and its id.
         RotationExpect(PersistentDkimRotation::InsertOrUpdate_(domainId, theirs), "an insert with no row there was not taken for a write");
         State read;
         bool exists = false;
         RotationExpect(PersistentDkimRotation::Read(domainId, read, exists) && exists && read.current.selector == "theirs",
            "an insert with no row there did not write the row");

         // The race, with its outcome fixed: the lookup has missed, and another
         // node has written the row since. The insert collides, and this pass's
         // state is what the row must say afterwards.
         RotationExpect(PersistentDkimRotation::InsertOrUpdate_(domainId, ours), "the update after a refused insert failed");
         RotationExpect(PersistentDkimRotation::Read(domainId, read, exists) && exists, "the row could not be read after the race");
         RotationExpect(read.current.selector == "ours" && read.current.since == 2000 && read.last_error == _T("ours"),
            "a refused insert was taken for a write: the row still holds the other node's state");
         RotationExpect(RotationRows(domainId) == 1, "the race left other than one row");

         // Save itself, over the row that is there: the update.
         RotationExpect(PersistentDkimRotation::Save(domainId, theirs), "Save over an existing row failed");
         RotationExpect(PersistentDkimRotation::Read(domainId, read, exists) && exists && read.current.selector == "theirs",
            "Save over an existing row did not update it");
      }
      catch (...)
      {
         DeleteRotationRows(domainId);
         PersistentDomain::DeleteObject(domain);
         throw;
      }

      DeleteRotationRows(domainId);
      PersistentDomain::DeleteObject(domain);
   }
}
