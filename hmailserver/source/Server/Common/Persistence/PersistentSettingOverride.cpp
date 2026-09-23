// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "PersistentSettingOverride.h"
#include "../Cache/CacheContainer.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/DALRecordset.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      String Lower(const String &name)
      {
         String lower = name;
         lower.ToLower();
         return lower;
      }

      // The row an owner holds for a name, or 0.
      __int64 FindRow(int scope, __int64 ownerId, const String &name)
      {
         SQLCommand command("select soid from hm_settingoverrides where soscope = @SCOPE and soownerid = @OWNERID and soname = @NAME");
         command.AddParameter("@SCOPE", scope);
         command.AddParameter("@OWNERID", ownerId);
         command.AddParameter("@NAME", Lower(name));

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset || recordset->IsEOF())
            return 0;

         return recordset->GetInt64Value("soid");
      }
   }

   bool
   PersistentSettingOverride::ReadOwner(int scope, __int64 ownerId, std::map<String, String> &values)
   {
      values.clear();

      SQLCommand command("select soname, sovalue from hm_settingoverrides where soscope = @SCOPE and soownerid = @OWNERID");
      command.AddParameter("@SCOPE", scope);
      command.AddParameter("@OWNERID", ownerId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         values[Lower(recordset->GetStringValue("soname"))] = recordset->GetStringValue("sovalue");
         recordset->MoveNext();
      }

      return true;
   }

   bool
   PersistentSettingOverride::Write(int scope, __int64 ownerId, const String &name, const String &value)
   {
      if (ownerId <= 0)
         return false;

      const __int64 existing = FindRow(scope, ownerId, name);

      SQLStatement statement;
      statement.SetTable("hm_settingoverrides");

      bool result = false;

      if (existing > 0)
      {
         statement.SetStatementType(SQLStatement::STUpdate);
         statement.AddColumn("sovalue", value);
         statement.SetWhereClause("soid = " + StringParser::IntToString(existing));
         result = Application::Instance()->GetDBManager()->Execute(statement);
      }
      else
      {
         __int64 made = 0;
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("soid");
         statement.AddColumn("soscope", (long) scope);
         statement.AddColumnInt64("soownerid", ownerId);
         statement.AddColumn("soname", Lower(name));
         statement.AddColumn("sovalue", value);
         result = Application::Instance()->GetDBManager()->Execute(statement, &made) && made > 0;
      }

      // Told whether or not the write succeeded: a failed write may still have
      // reached the table, and a cache that is wrong in the direction of reading
      // again costs one query.
      CacheContainer::Instance()->RemoveSettingOverrides(scope, ownerId);

      return result;
   }

   bool
   PersistentSettingOverride::Remove(int scope, __int64 ownerId, const String &name)
   {
      SQLCommand command("delete from hm_settingoverrides where soscope = @SCOPE and soownerid = @OWNERID and soname = @NAME");
      command.AddParameter("@SCOPE", scope);
      command.AddParameter("@OWNERID", ownerId);
      command.AddParameter("@NAME", Lower(name));

      const bool result = Application::Instance()->GetDBManager()->Execute(command);

      CacheContainer::Instance()->RemoveSettingOverrides(scope, ownerId);

      return result;
   }

   bool
   PersistentSettingOverride::DeleteByOwner(int scope, __int64 ownerId)
   {
      SQLCommand command("delete from hm_settingoverrides where soscope = @SCOPE and soownerid = @OWNERID");
      command.AddParameter("@SCOPE", scope);
      command.AddParameter("@OWNERID", ownerId);

      const bool result = Application::Instance()->GetDBManager()->Execute(command);

      CacheContainer::Instance()->RemoveSettingOverrides(scope, ownerId);

      return result;
   }

   bool
   PersistentSettingOverride::ValuesOfName(const String &name, std::vector<String> &values)
   {
      values.clear();

      SQLCommand command("select sovalue from hm_settingoverrides where soname = @NAME");
      command.AddParameter("@NAME", Lower(name));

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         values.push_back(recordset->GetStringValue("sovalue"));
         recordset->MoveNext();
      }

      return true;
   }

   bool
   PersistentSettingOverride::OwnersOfName(int scope, const String &name, std::map<__int64, String> &owners)
   {
      owners.clear();

      SQLCommand command("select soownerid, sovalue from hm_settingoverrides where soscope = @SCOPE and soname = @NAME");
      command.AddParameter("@SCOPE", scope);
      command.AddParameter("@NAME", Lower(name));

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         owners[recordset->GetInt64Value("soownerid")] = recordset->GetStringValue("sovalue");
         recordset->MoveNext();
      }

      return true;
   }
}
