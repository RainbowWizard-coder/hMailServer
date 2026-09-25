// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See CalendarStore.h. Reads and writes go through the same SQLCommand and
// SQLStatement path as every other table, parameterised. Every write that
// changes a collection steps its token and stamps the row with the new value
// under one process-wide mutex, so two writers cannot each read the same
// token and leave a row the sync report never answers.

#include "StdAfx.h"
#include "CalendarStore.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DALRecordset.h"
#include "../Application/Application.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const char *CalendarStore::DefaultName = "calendar";
   const char *CalendarStore::DefaultDisplayName = "Calendar";
   const int CalendarStore::MaximumUriLength;

   namespace
   {
      boost::recursive_mutex write_mutex;

      const char *CalendarColumns =
         "calendarid, calendaraccountid, calendarname, calendardisplayname, calendarsynctoken, calendarcreated, calendarcolor";

      const char *ObjectColumns =
         "objectid, objectcalendarid, objecturi, objectuid, objectcomponent, objectdata, objectetag, objectsynctoken, "
         "objectstart, objectend, objectfirst, objectlast, objectdeleted, objectmodified";

      AnsiString Int64Text(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      __int64 Now()
      {
         return static_cast<__int64>(time(nullptr));
      }

      void ReadCalendar(std::shared_ptr<DALRecordset> recordset, CalendarRecord &calendar)
      {
         calendar.id = recordset->GetInt64Value("calendarid");
         calendar.accountId = recordset->GetInt64Value("calendaraccountid");
         calendar.name = recordset->GetStringValue("calendarname");
         calendar.displayName = recordset->GetStringValue("calendardisplayname");
         calendar.syncToken = recordset->GetInt64Value("calendarsynctoken");
         calendar.created = recordset->GetInt64Value("calendarcreated");
         calendar.color = recordset->GetStringValue("calendarcolor");
      }

      const char *ShareColumns =
         "shareid, sharecalendarid, shareaccountid, sharegroupid, sharewrite, sharedelegate";

      void ReadShare(std::shared_ptr<DALRecordset> recordset, CalendarShareRecord &share)
      {
         share.id = recordset->GetInt64Value("shareid");
         share.calendarId = recordset->GetInt64Value("sharecalendarid");
         share.accountId = recordset->GetInt64Value("shareaccountid");
         share.groupId = recordset->GetInt64Value("sharegroupid");
         share.delegate = recordset->GetInt64Value("sharedelegate") != 0;
         // A delegate may write, whatever the write column says: the two are
         // written together, and a row an older build wrote has neither.
         share.write = recordset->GetInt64Value("sharewrite") != 0 || share.delegate;
      }

      // The rows that reach an account: its own, and those of every group it
      // is a member of, each joined to its calendar. A calendar may come back
      // more than once; the callers merge.
      bool ReadReaching(__int64 accountId, std::vector<CalendarRecord> &calendars, std::vector<CalendarShareRecord> &shares)
      {
         calendars.clear();
         shares.clear();

         SQLCommand direct(String(AnsiString("select ") + ShareColumns + ", " + CalendarColumns +
            " from hm_calendarshares inner join hm_calendars on calendarid = sharecalendarid where shareaccountid = @ACCOUNTID and sharegroupid = 0 order by calendardisplayname, calendarid"));
         direct.AddParameter("@ACCOUNTID", accountId);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(direct);
         if (!recordset)
            return false;
         while (!recordset->IsEOF())
         {
            CalendarRecord calendar;
            ReadCalendar(recordset, calendar);
            CalendarShareRecord share;
            ReadShare(recordset, share);
            calendars.push_back(calendar);
            shares.push_back(share);
            recordset->MoveNext();
         }

         // Through a group: the membership rows joined in, so the answer is
         // what the groups say now. A join rather than a subquery, since SQL
         // Server Compact is happier with it. The account's own calendar,
         // reached through a group it is a member of, is not a share: it would
         // be listed a second time, read-only, beneath itself.
         SQLCommand viaGroup(String(AnsiString("select ") + ShareColumns + ", " + CalendarColumns +
            " from hm_calendarshares inner join hm_calendars on calendarid = sharecalendarid inner join hm_group_members on membergroupid = sharegroupid where memberaccountid = @ACCOUNTID and sharegroupid <> 0 and calendaraccountid <> @OWNERID order by calendardisplayname, calendarid"));
         viaGroup.AddParameter("@ACCOUNTID", accountId);
         viaGroup.AddParameter("@OWNERID", accountId);

         recordset = Application::Instance()->GetDBManager()->OpenRecordset(viaGroup);
         if (!recordset)
            return false;
         while (!recordset->IsEOF())
         {
            CalendarRecord calendar;
            ReadCalendar(recordset, calendar);
            CalendarShareRecord share;
            ReadShare(recordset, share);
            calendars.push_back(calendar);
            shares.push_back(share);
            recordset->MoveNext();
         }
         return true;
      }

      // One entry per calendar: the account's own row when there is one, else
      // the first group's, carrying the widest rights of every row that reaches
      // the calendar.
      void MergeReaching(const std::vector<CalendarRecord> &calendars, const std::vector<CalendarShareRecord> &shares,
                         std::vector<CalendarRecord> &mergedCalendars, std::vector<CalendarShareRecord> &mergedShares)
      {
         mergedCalendars.clear();
         mergedShares.clear();
         for (size_t i = 0; i < calendars.size(); i++)
         {
            size_t at = mergedCalendars.size();
            for (size_t j = 0; j < mergedCalendars.size(); j++)
            {
               if (mergedCalendars[j].id == calendars[i].id)
               {
                  at = j;
                  break;
               }
            }
            if (at == mergedCalendars.size())
            {
               mergedCalendars.push_back(calendars[i]);
               mergedShares.push_back(shares[i]);
               continue;
            }
            CalendarShareRecord &kept = mergedShares[at];
            if (kept.accountId == 0 && shares[i].accountId != 0)
            {
               bool write = kept.write;
               bool delegate = kept.delegate;
               kept = shares[i];
               kept.write = kept.write || write;
               kept.delegate = kept.delegate || delegate;
            }
            else
            {
               kept.write = kept.write || shares[i].write;
               kept.delegate = kept.delegate || shares[i].delegate;
            }
         }
      }

      void ReadObject(std::shared_ptr<DALRecordset> recordset, CalendarObjectRecord &object)
      {
         object.id = recordset->GetInt64Value("objectid");
         object.calendarId = recordset->GetInt64Value("objectcalendarid");
         object.uri = recordset->GetStringValue("objecturi");
         object.uid = recordset->GetStringValue("objectuid");
         object.component = recordset->GetStringValue("objectcomponent");
         object.data = recordset->GetStringValue("objectdata");
         object.etag = recordset->GetStringValue("objectetag");
         object.syncToken = recordset->GetInt64Value("objectsynctoken");
         object.start = recordset->GetInt64Value("objectstart");
         object.end = recordset->GetInt64Value("objectend");
         object.first = recordset->GetInt64Value("objectfirst");
         object.last = recordset->GetInt64Value("objectlast");
         object.deleted = recordset->GetLongValue("objectdeleted") != 0;
         object.modified = recordset->GetInt64Value("objectmodified");
      }

      bool ReadObjects(const SQLCommand &command, std::vector<CalendarObjectRecord> &objects)
      {
         objects.clear();

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;

         while (!recordset->IsEOF())
         {
            CalendarObjectRecord object;
            ReadObject(recordset, object);
            objects.push_back(object);
            recordset->MoveNext();
         }

         return true;
      }

      bool ReadOneObject(const SQLCommand &command, CalendarObjectRecord &object)
      {
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset || recordset->IsEOF())
            return false;

         ReadObject(recordset, object);
         return true;
      }

      // The row whose resource name, or UID, is exactly the one asked for. The WHERE
      // clause compares under the column's collation, which ignores case on SQL
      // Server and Compact and case, accents and trailing spaces on MySQL, while a
      // CalDAV resource name (an RFC 3986 path segment) and an iCalendar UID are
      // case-sensitive. So the database narrows the candidates and this decides:
      // "Event.ics" and "event.ics" are two objects, and a PUT to one must never
      // overwrite the other. Every row is walked, because no unique index covers
      // UIDs.
      bool ReadExactObject(const SQLCommand &command, bool byUid, const String &value, CalendarObjectRecord &object)
      {
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;

         while (!recordset->IsEOF())
         {
            CalendarObjectRecord candidate;
            ReadObject(recordset, candidate);

            if ((byUid ? candidate.uid : candidate.uri).Compare(value) == 0)
            {
               object = candidate;
               return true;
            }

            recordset->MoveNext();
         }

         return false;
      }

      // The collection's counter stepped by one, and the new value read
      // back. Called with write_mutex held.
      bool StepToken(__int64 accountId, __int64 calendarId, __int64 &token)
      {
         SQLCommand step("update hm_calendars set calendarsynctoken = calendarsynctoken + 1 where calendarid = @ID and calendaraccountid = @ACCOUNTID");
         step.AddParameter("@ID", calendarId);
         step.AddParameter("@ACCOUNTID", accountId);
         if (!Application::Instance()->GetDBManager()->Execute(step))
            return false;

         CalendarRecord calendar;
         if (!CalendarStore::Get(accountId, calendarId, calendar))
            return false;

         token = calendar.syncToken;
         return true;
      }

      void AddObjectColumns(SQLStatement &statement, const CalendarObjectRecord &object, __int64 token)
      {
         statement.AddColumn("objectuid", object.uid);
         statement.AddColumn("objectcomponent", object.component);
         statement.AddColumn("objectdata", object.data);
         statement.AddColumn("objectetag", object.etag);
         statement.AddColumnInt64("objectsynctoken", token);
         statement.AddColumnInt64("objectstart", object.start);
         statement.AddColumnInt64("objectend", object.end);
         statement.AddColumnInt64("objectfirst", object.first);
         statement.AddColumnInt64("objectlast", object.last);
         statement.AddColumnInt64("objectdeleted", 0);
         statement.AddColumnInt64("objectmodified", Now());
      }
   }

   bool
   CalendarStore::EnsureDefault(__int64 accountId, CalendarRecord &calendar)
   {
      String name = DefaultName;

      SQLCommand select(String(AnsiString("select ") + CalendarColumns + " from hm_calendars where calendaraccountid = @ACCOUNTID and calendarname = @NAME"));
      select.AddParameter("@ACCOUNTID", accountId);
      select.AddParameter("@NAME", name);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(select);
      if (!recordset)
         return false;

      if (!recordset->IsEOF())
      {
         ReadCalendar(recordset, calendar);
         return true;
      }

      // Made on first use, once: a second request for the same account at the
      // same moment waits here and then finds the row.
      boost::lock_guard<boost::recursive_mutex> guard(write_mutex);

      recordset = Application::Instance()->GetDBManager()->OpenRecordset(select);
      if (!recordset)
         return false;
      if (!recordset->IsEOF())
      {
         ReadCalendar(recordset, calendar);
         return true;
      }

      calendar = CalendarRecord();
      calendar.accountId = accountId;
      calendar.name = name;
      calendar.displayName = DefaultDisplayName;
      calendar.syncToken = 1;
      calendar.created = Now();

      SQLStatement statement;
      statement.SetTable("hm_calendars");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("calendarid");
      statement.AddColumnInt64("calendaraccountid", accountId);
      statement.AddColumn("calendarname", calendar.name);
      statement.AddColumn("calendardisplayname", calendar.displayName);
      statement.AddColumnInt64("calendarsynctoken", calendar.syncToken);
      statement.AddColumnInt64("calendarcreated", calendar.created);
      statement.AddColumn("calendarcolor", calendar.color);

      __int64 id = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id <= 0)
         return false;

      calendar.id = id;
      return true;
   }

   bool
   CalendarStore::GetAny(__int64 calendarId, CalendarRecord &calendar)
   {
      SQLCommand command(String(AnsiString("select ") + CalendarColumns + " from hm_calendars where calendarid = @ID"));
      command.AddParameter("@ID", calendarId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadCalendar(recordset, calendar);
      return true;
   }

   bool
   CalendarStore::ListCalendars(__int64 accountId, std::vector<CalendarRecord> &calendars)
   {
      calendars.clear();

      CalendarRecord first;
      if (!EnsureDefault(accountId, first))
         return false;
      calendars.push_back(first);

      SQLCommand command(String(AnsiString("select ") + CalendarColumns + " from hm_calendars where calendaraccountid = @ACCOUNTID and calendarname <> @DEFAULT order by calendardisplayname, calendarname"));
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@DEFAULT", String(DefaultName));

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         CalendarRecord calendar;
         ReadCalendar(recordset, calendar);
         calendars.push_back(calendar);
         recordset->MoveNext();
      }
      return true;
   }

   bool
   CalendarStore::ListCalendarsIfAny(__int64 accountId, std::vector<CalendarRecord> &calendars)
   {
      calendars.clear();

      SQLCommand command(String(AnsiString("select ") + CalendarColumns + " from hm_calendars where calendaraccountid = @ACCOUNTID order by calendarname"));
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         CalendarRecord calendar;
         ReadCalendar(recordset, calendar);
         calendars.push_back(calendar);
         recordset->MoveNext();
      }
      return true;
   }

   bool
   CalendarStore::FindByName(__int64 accountId, const String &name, CalendarRecord &calendar)
   {
      if (name.IsEmpty() || name.GetLength() > 255)
         return false;

      if (name.CompareNoCase(String(DefaultName)) == 0)
         return EnsureDefault(accountId, calendar);

      SQLCommand command(String(AnsiString("select ") + CalendarColumns + " from hm_calendars where calendaraccountid = @ACCOUNTID and calendarname = @NAME"));
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@NAME", name);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadCalendar(recordset, calendar);
      return true;
   }

   bool
   CalendarStore::IsUsableCalendarName(const String &name)
   {
      if (name.IsEmpty() || name.GetLength() > 255)
         return false;
      for (int i = 0; i < name.GetLength(); i++)
      {
         wchar_t c = name.c_str()[i];
         if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == '%' || c == '?' || c == '#')
            return false;
      }
      if (name == _T(".") || name == _T(".."))
         return false;
      return !name.StartsWith(_T("shared-"));
   }

   bool
   CalendarStore::CreateCalendar(__int64 accountId, const String &name, const String &displayName, const String &color, CalendarRecord &created)
   {
      if (!IsUsableCalendarName(name))
         return false;

      boost::lock_guard<boost::recursive_mutex> guard(write_mutex);

      CalendarRecord taken;
      if (FindByName(accountId, name, taken))
         return false;

      created = CalendarRecord();
      created.accountId = accountId;
      created.name = name;
      created.displayName = displayName.IsEmpty() ? name : displayName;
      created.color = color;
      created.syncToken = 1;
      created.created = Now();

      SQLStatement statement;
      statement.SetTable("hm_calendars");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("calendarid");
      statement.AddColumnInt64("calendaraccountid", accountId);
      statement.AddColumn("calendarname", created.name);
      statement.AddColumn("calendardisplayname", created.displayName);
      statement.AddColumnInt64("calendarsynctoken", created.syncToken);
      statement.AddColumnInt64("calendarcreated", created.created);
      statement.AddColumn("calendarcolor", created.color);

      __int64 id = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id <= 0)
         return false;

      created.id = id;
      return true;
   }

   bool
   CalendarStore::UpdateCalendar(__int64 accountId, __int64 calendarId, const String &displayName, const String &color)
   {
      boost::lock_guard<boost::recursive_mutex> guard(write_mutex);

      CalendarRecord calendar;
      if (!Get(accountId, calendarId, calendar))
         return false;

      SQLStatement statement;
      statement.SetTable("hm_calendars");
      statement.SetStatementType(SQLStatement::STUpdate);
      statement.AddColumn("calendardisplayname", displayName.IsEmpty() ? calendar.displayName : displayName);
      statement.AddColumn("calendarcolor", color);
      statement.AddColumnInt64("calendarsynctoken", calendar.syncToken + 1);
      statement.SetWhereClause("calendarid = " + Int64Text(calendarId) + " and calendaraccountid = " + Int64Text(accountId));
      return Application::Instance()->GetDBManager()->Execute(statement);
   }

   bool
   CalendarStore::DeleteCalendar(__int64 accountId, __int64 calendarId)
   {
      boost::lock_guard<boost::recursive_mutex> guard(write_mutex);

      CalendarRecord calendar;
      if (!Get(accountId, calendarId, calendar))
         return false;
      if (calendar.name.CompareNoCase(String(DefaultName)) == 0)
         return false;

      // The objects and the shares first: the foreign keys cascade on every
      // backend but the rows are removed here as well, so a database made
      // without them (an upgrade that failed half way) is left clean.
      SQLCommand objects("delete from hm_calendarobjects where objectcalendarid = @ID");
      objects.AddParameter("@ID", calendarId);
      if (!Application::Instance()->GetDBManager()->Execute(objects))
         return false;

      SQLCommand shares("delete from hm_calendarshares where sharecalendarid = @ID");
      shares.AddParameter("@ID", calendarId);
      if (!Application::Instance()->GetDBManager()->Execute(shares))
         return false;

      SQLCommand row("delete from hm_calendars where calendarid = @ID and calendaraccountid = @ACCOUNTID");
      row.AddParameter("@ID", calendarId);
      row.AddParameter("@ACCOUNTID", accountId);
      return Application::Instance()->GetDBManager()->Execute(row);
   }

   bool
   CalendarStore::DefaultIfAny(__int64 accountId, CalendarRecord &calendar)
   {
      SQLCommand select(String(AnsiString("select ") + CalendarColumns + " from hm_calendars where calendaraccountid = @ACCOUNTID and calendarname = @NAME"));
      select.AddParameter("@ACCOUNTID", accountId);
      select.AddParameter("@NAME", String(DefaultName));

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(select);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadCalendar(recordset, calendar);
      return true;
   }

   bool
   CalendarStore::ListShares(__int64 calendarId, std::vector<CalendarShareRecord> &shares)
   {
      shares.clear();

      SQLCommand accounts(String(AnsiString("select ") + ShareColumns + ", accountaddress from hm_calendarshares inner join hm_accounts on accountid = shareaccountid where sharecalendarid = @ID and sharegroupid = 0 order by accountaddress"));
      accounts.AddParameter("@ID", calendarId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(accounts);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         CalendarShareRecord share;
         ReadShare(recordset, share);
         share.address = recordset->GetStringValue("accountaddress");
         shares.push_back(share);
         recordset->MoveNext();
      }

      // The groups, each with its name and its size. The join is a left one so
      // that a row whose group has gone is seen - and dropped here, since no
      // key drops it.
      SQLCommand groups(String(AnsiString("select ") + ShareColumns + ", groupname from hm_calendarshares left outer join hm_groups on groupid = sharegroupid where sharecalendarid = @ID and sharegroupid <> 0 order by groupname"));
      groups.AddParameter("@ID", calendarId);

      recordset = Application::Instance()->GetDBManager()->OpenRecordset(groups);
      if (!recordset)
         return false;

      std::vector<__int64> orphans;
      while (!recordset->IsEOF())
      {
         CalendarShareRecord share;
         ReadShare(recordset, share);
         share.groupName = recordset->GetStringValue("groupname");
         if (share.groupName.IsEmpty())
            orphans.push_back(share.id);
         else
            shares.push_back(share);
         recordset->MoveNext();
      }

      for (size_t i = 0; i < orphans.size(); i++)
      {
         SQLCommand drop("delete from hm_calendarshares where shareid = @ID");
         drop.AddParameter("@ID", orphans[i]);
         Application::Instance()->GetDBManager()->Execute(drop);
      }

      for (size_t i = 0; i < shares.size(); i++)
      {
         if (shares[i].groupId == 0)
            continue;
         SQLCommand count("select count(*) as membercount from hm_group_members where membergroupid = @GROUPID");
         count.AddParameter("@GROUPID", shares[i].groupId);
         std::shared_ptr<DALRecordset> counted = Application::Instance()->GetDBManager()->OpenRecordset(count);
         if (counted && !counted->IsEOF())
            shares[i].members = static_cast<int>(counted->GetLongValue("membercount"));
      }
      return true;
   }

   bool
   CalendarStore::ListSharedWith(__int64 accountId, std::vector<CalendarRecord> &calendars, std::vector<CalendarShareRecord> &shares)
   {
      std::vector<CalendarRecord> reachingCalendars;
      std::vector<CalendarShareRecord> reachingShares;
      if (!ReadReaching(accountId, reachingCalendars, reachingShares))
         return false;

      MergeReaching(reachingCalendars, reachingShares, calendars, shares);
      return true;
   }

   bool
   CalendarStore::ShareOf(__int64 calendarId, __int64 accountId, CalendarShareRecord &share)
   {
      // The rows that reach the account, narrowed to the one calendar: a
      // calendar is shared with a handful of accounts and an account is
      // shared a handful of calendars, so the walk is short either way.
      std::vector<CalendarRecord> calendars;
      std::vector<CalendarShareRecord> shares;
      if (!ListSharedWith(accountId, calendars, shares))
         return false;

      for (size_t i = 0; i < calendars.size(); i++)
      {
         if (calendars[i].id == calendarId)
         {
            share = shares[i];
            return true;
         }
      }
      return false;
   }

   bool
   CalendarStore::DirectShareOf(__int64 calendarId, __int64 accountId, CalendarShareRecord &share)
   {
      SQLCommand command(String(AnsiString("select ") + ShareColumns + " from hm_calendarshares where sharecalendarid = @ID and shareaccountid = @ACCOUNTID and sharegroupid = 0"));
      command.AddParameter("@ID", calendarId);
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadShare(recordset, share);
      return true;
   }

   bool
   CalendarStore::GroupShareOf(__int64 calendarId, __int64 groupId, CalendarShareRecord &share)
   {
      SQLCommand command(String(AnsiString("select ") + ShareColumns + " from hm_calendarshares where sharecalendarid = @ID and sharegroupid = @GROUPID"));
      command.AddParameter("@ID", calendarId);
      command.AddParameter("@GROUPID", groupId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadShare(recordset, share);
      return true;
   }

   bool
   CalendarStore::Share(__int64 calendarId, __int64 accountId, __int64 groupId, bool write, bool delegate)
   {
      if ((accountId == 0) == (groupId == 0))
         return false;
      if (delegate)
         write = true;

      boost::lock_guard<boost::recursive_mutex> guard(write_mutex);

      CalendarShareRecord existing;
      bool found = groupId != 0 ? GroupShareOf(calendarId, groupId, existing) : DirectShareOf(calendarId, accountId, existing);
      if (found)
      {
         SQLStatement statement;
         statement.SetTable("hm_calendarshares");
         statement.SetStatementType(SQLStatement::STUpdate);
         statement.AddColumnInt64("sharewrite", write ? 1 : 0);
         statement.AddColumnInt64("sharedelegate", delegate ? 1 : 0);
         statement.SetWhereClause("shareid = " + Int64Text(existing.id));
         return Application::Instance()->GetDBManager()->Execute(statement);
      }

      SQLStatement statement;
      statement.SetTable("hm_calendarshares");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("shareid");
      statement.AddColumnInt64("sharecalendarid", calendarId);
      statement.AddColumnInt64("shareaccountid", accountId);
      statement.AddColumnInt64("sharegroupid", groupId);
      statement.AddColumnInt64("sharewrite", write ? 1 : 0);
      statement.AddColumnInt64("sharedelegate", delegate ? 1 : 0);

      __int64 id = 0;
      return Application::Instance()->GetDBManager()->Execute(statement, &id) && id > 0;
   }

   bool
   CalendarStore::Unshare(__int64 calendarId, __int64 accountId)
   {
      boost::lock_guard<boost::recursive_mutex> guard(write_mutex);

      SQLCommand command("delete from hm_calendarshares where sharecalendarid = @ID and shareaccountid = @ACCOUNTID and sharegroupid = 0");
      command.AddParameter("@ID", calendarId);
      command.AddParameter("@ACCOUNTID", accountId);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   CalendarStore::UnshareGroup(__int64 calendarId, __int64 groupId)
   {
      boost::lock_guard<boost::recursive_mutex> guard(write_mutex);

      SQLCommand command("delete from hm_calendarshares where sharecalendarid = @ID and sharegroupid = @GROUPID");
      command.AddParameter("@ID", calendarId);
      command.AddParameter("@GROUPID", groupId);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   CalendarStore::UnshareAll(__int64 accountId)
   {
      SQLCommand command("delete from hm_calendarshares where shareaccountid = @ACCOUNTID and sharegroupid = 0");
      command.AddParameter("@ACCOUNTID", accountId);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   CalendarStore::UnshareAllForGroup(__int64 groupId)
   {
      if (groupId == 0)
         return true;
      SQLCommand command("delete from hm_calendarshares where sharegroupid = @GROUPID");
      command.AddParameter("@GROUPID", groupId);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   CalendarStore::Get(__int64 accountId, __int64 calendarId, CalendarRecord &calendar)
   {
      SQLCommand command(String(AnsiString("select ") + CalendarColumns + " from hm_calendars where calendarid = @ID and calendaraccountid = @ACCOUNTID"));
      command.AddParameter("@ID", calendarId);
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      ReadCalendar(recordset, calendar);
      return true;
   }

   bool
   CalendarStore::List(__int64 accountId, __int64 calendarId, std::vector<CalendarObjectRecord> &objects)
   {
      SQLCommand command(String(AnsiString("select ") + ObjectColumns + " from hm_calendarobjects where objectaccountid = @ACCOUNTID and objectcalendarid = @CALENDARID and objectdeleted = 0 order by objecturi asc"));
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@CALENDARID", calendarId);
      return ReadObjects(command, objects);
   }

   bool
   CalendarStore::ListInRange(__int64 accountId, __int64 calendarId, __int64 rangeStart, __int64 rangeEnd, std::vector<CalendarObjectRecord> &objects)
   {
      SQLCommand command(String(AnsiString("select ") + ObjectColumns + " from hm_calendarobjects where objectaccountid = @ACCOUNTID and objectcalendarid = @CALENDARID and objectdeleted = 0 and objectlast > @RANGESTART and objectfirst < @RANGEEND order by objecturi asc"));
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@CALENDARID", calendarId);
      command.AddParameter("@RANGESTART", rangeStart);
      command.AddParameter("@RANGEEND", rangeEnd);
      return ReadObjects(command, objects);
   }

   bool
   CalendarStore::ListChangedSince(__int64 accountId, __int64 calendarId, __int64 sinceToken, std::vector<CalendarObjectRecord> &objects)
   {
      SQLCommand command(String(AnsiString("select ") + ObjectColumns + " from hm_calendarobjects where objectaccountid = @ACCOUNTID and objectcalendarid = @CALENDARID and objectsynctoken > @SINCE order by objecturi asc"));
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@CALENDARID", calendarId);
      command.AddParameter("@SINCE", sinceToken);
      return ReadObjects(command, objects);
   }

   bool
   CalendarStore::FindByUri(__int64 accountId, __int64 calendarId, const String &uri, CalendarObjectRecord &object)
   {
      // objecturi is nvarchar(255): a longer name from a request path cannot exist.
      if (uri.IsEmpty() || uri.GetLength() > 255)
         return false;

      SQLCommand command(String(AnsiString("select ") + ObjectColumns + " from hm_calendarobjects where objectaccountid = @ACCOUNTID and objectcalendarid = @CALENDARID and objecturi = @URI and objectdeleted = 0"));
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@CALENDARID", calendarId);
      command.AddParameter("@URI", uri);
      return ReadExactObject(command, false, uri, object);
   }

   bool
   CalendarStore::FindByUid(__int64 accountId, __int64 calendarId, const String &uid, CalendarObjectRecord &object)
   {
      if (uid.IsEmpty() || uid.GetLength() > 255)
         return false;

      SQLCommand command(String(AnsiString("select ") + ObjectColumns + " from hm_calendarobjects where objectaccountid = @ACCOUNTID and objectcalendarid = @CALENDARID and objectuid = @UID and objectdeleted = 0"));
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@CALENDARID", calendarId);
      command.AddParameter("@UID", uid);
      return ReadExactObject(command, true, uid, object);
   }

   bool
   CalendarStore::FindByUidAmong(__int64 accountId, const std::vector<__int64> &calendarIds, const String &uid,
                                 CalendarObjectRecord &object, bool &failed)
   {
      failed = false;
      if (uid.IsEmpty() || uid.GetLength() > 255 || calendarIds.empty())
         return false;

      // The ids are numbers of this server's own: written into the text, the
      // UID a parameter.
      AnsiString among;
      for (size_t i = 0; i < calendarIds.size(); i++)
         among += (i == 0 ? AnsiString() : AnsiString(",")) + Int64Text(calendarIds[i]);

      SQLCommand command(String(AnsiString("select ") + ObjectColumns + " from hm_calendarobjects where objectcalendarid in (" + among +
                                ") and objectaccountid = @ACCOUNTID and objectuid = @UID and objectdeleted = 0"));
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@UID", uid);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
      {
         failed = true;
         return false;
      }

      while (!recordset->IsEOF())
      {
         CalendarObjectRecord candidate;
         ReadObject(recordset, candidate);
         if (candidate.uid.Compare(uid) == 0)
         {
            object = candidate;
            return true;
         }
         recordset->MoveNext();
      }
      return false;
   }

   bool
   CalendarStore::FindById(__int64 objectId, CalendarObjectRecord &object)
   {
      SQLCommand command(String(AnsiString("select ") + ObjectColumns + " from hm_calendarobjects where objectid = @ID and objectdeleted = 0"));
      command.AddParameter("@ID", objectId);
      return ReadOneObject(command, object);
   }

   bool
   CalendarStore::FindCollationTwin(__int64 accountId, __int64 calendarId, const String &uri, CalendarObjectRecord &object)
   {
      if (uri.IsEmpty() || uri.GetLength() > 255)
         return false;

      // Live objects only, as the header says: a deleted twin is revived by Insert
      // under the new spelling rather than blocking the name.
      SQLCommand command(String(AnsiString("select ") + ObjectColumns + " from hm_calendarobjects where objectaccountid = @ACCOUNTID and objectcalendarid = @CALENDARID and objecturi = @URI and objectdeleted = 0"));
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@CALENDARID", calendarId);
      command.AddParameter("@URI", uri);

      CalendarObjectRecord candidate;
      if (!ReadOneObject(command, candidate) || candidate.uri.Compare(uri) == 0)
         return false;

      object = candidate;
      return true;
   }

   bool
   CalendarStore::Insert(__int64 accountId, __int64 calendarId, const CalendarObjectRecord &object, CalendarObjectRecord &inserted, __int64 &token)
   {
      boost::lock_guard<boost::recursive_mutex> guard(write_mutex);

      if (!StepToken(accountId, calendarId, token))
         return false;

      // A tombstone under this name: the row comes back rather than a second
      // row, since the unique index on the name is what keeps a sync honest.
      SQLCommand find(String(AnsiString("select ") + ObjectColumns + " from hm_calendarobjects where objectaccountid = @ACCOUNTID and objectcalendarid = @CALENDARID and objecturi = @URI"));
      find.AddParameter("@ACCOUNTID", accountId);
      find.AddParameter("@CALENDARID", calendarId);
      find.AddParameter("@URI", object.uri);
      CalendarObjectRecord existing;
      if (ReadOneObject(find, existing))
      {
         // Found under the collation. A LIVE row that is not exactly this name - a
         // case or accent twin - is a different object the unique index will not let
         // a second row sit beside, so the insert is refused and the caller answers
         // 409. A deleted one is only a tombstone holding the name: it is revived
         // under the spelling asked for, which is what reviving a tombstone of the
         // exact name has always done; a client whose sync token predates the delete
         // then sees the object under its new name rather than the old one gone.
         if (!existing.deleted)
            return false;

         SQLStatement statement;
         statement.SetTable("hm_calendarobjects");
         statement.SetStatementType(SQLStatement::STUpdate);
         AddObjectColumns(statement, object, token);
         if (existing.uri.Compare(object.uri) != 0)
            statement.AddColumn("objecturi", object.uri);
         statement.SetWhereClause("objectid = " + Int64Text(existing.id) + " and objectaccountid = " + Int64Text(accountId));
         if (!Application::Instance()->GetDBManager()->Execute(statement))
            return false;

         inserted = object;
         inserted.id = existing.id;
         inserted.calendarId = calendarId;
         inserted.syncToken = token;
         inserted.deleted = false;
         inserted.modified = Now();
         return true;
      }

      SQLStatement statement;
      statement.SetTable("hm_calendarobjects");
      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn("objectid");
      statement.AddColumnInt64("objectaccountid", accountId);
      statement.AddColumnInt64("objectcalendarid", calendarId);
      statement.AddColumn("objecturi", object.uri);
      AddObjectColumns(statement, object, token);

      __int64 id = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id <= 0)
         return false;

      inserted = object;
      inserted.id = id;
      inserted.calendarId = calendarId;
      inserted.syncToken = token;
      inserted.deleted = false;
      inserted.modified = Now();
      return true;
   }

   bool
   CalendarStore::Update(__int64 accountId, __int64 calendarId, __int64 objectId, const CalendarObjectRecord &object, __int64 &token)
   {
      boost::lock_guard<boost::recursive_mutex> guard(write_mutex);

      if (!StepToken(accountId, calendarId, token))
         return false;

      SQLStatement statement;
      statement.SetTable("hm_calendarobjects");
      statement.SetStatementType(SQLStatement::STUpdate);
      AddObjectColumns(statement, object, token);
      statement.SetWhereClause("objectid = " + Int64Text(objectId) + " and objectaccountid = " + Int64Text(accountId) + " and objectcalendarid = " + Int64Text(calendarId));

      return Application::Instance()->GetDBManager()->Execute(statement);
   }

   bool
   CalendarStore::Delete(__int64 accountId, __int64 calendarId, __int64 objectId, __int64 &token)
   {
      boost::lock_guard<boost::recursive_mutex> guard(write_mutex);

      SQLCommand select("select objectid from hm_calendarobjects where objectid = @ID and objectaccountid = @ACCOUNTID and objectcalendarid = @CALENDARID and objectdeleted = 0");
      select.AddParameter("@ID", objectId);
      select.AddParameter("@ACCOUNTID", accountId);
      select.AddParameter("@CALENDARID", calendarId);
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(select);
      if (!recordset || recordset->IsEOF())
         return false;

      if (!StepToken(accountId, calendarId, token))
         return false;

      // The tombstone keeps its name and UID and nothing else: the text goes,
      // and the span is emptied so no time-range query reads the row.
      SQLStatement statement;
      statement.SetTable("hm_calendarobjects");
      statement.SetStatementType(SQLStatement::STUpdate);
      statement.AddColumn("objectdata", String());
      statement.AddColumn("objectetag", String());
      statement.AddColumnInt64("objectsynctoken", token);
      statement.AddColumnInt64("objectstart", 0);
      statement.AddColumnInt64("objectend", 0);
      statement.AddColumnInt64("objectfirst", 0);
      statement.AddColumnInt64("objectlast", 0);
      statement.AddColumnInt64("objectdeleted", 1);
      statement.AddColumnInt64("objectmodified", Now());
      statement.SetWhereClause("objectid = " + Int64Text(objectId) + " and objectaccountid = " + Int64Text(accountId));

      return Application::Instance()->GetDBManager()->Execute(statement);
   }
}
