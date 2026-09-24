// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "IpWarmup.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The standard plan's ceilings for days 1 to 7; days 8 on are ranges.
      const int FirstWeek[7] = { 50, 100, 200, 500, 1000, 2000, 5000 };

      // The standard plan, which the other two are written from: the daily
      // ceiling for one named provider on a warm-up day, or -1 after day 30.
      int StandardCeiling(int day)
      {
         if (day < 1)
            day = 1;

         if (day <= 7)
            return FirstWeek[day - 1];

         if (day <= 10)
            return 10000;

         if (day <= 14)
            return 20000;

         if (day <= 21)
            return 50000;

         if (day <= 30)
            return 100000;

         return -1;
      }

      // The longest address the column holds (hm_warmup.warmupaddress).
      const int AddressMaximum = 64;
   }

   IpWarmup::IpWarmup() :
      schedules_loaded_(false)
   {

   }

   IpWarmup::~IpWarmup()
   {

   }

   // ---- the arithmetic ----------------------------------------------------

   __int64
   IpWarmup::DayOf(__int64 moment)
   {
      if (moment < 0)
         return 0;

      return moment / DaySeconds;
   }

   int
   IpWarmup::WarmupDay(__int64 started, __int64 today)
   {
      if (today < started)
         return 1;

      const __int64 day = today - started + 1;

      // Far past any plan's last day; kept inside an int.
      if (day > 100000)
         return 100000;

      return (int) day;
   }

   int
   IpWarmup::BaseCeiling(int plan, int warmupDay)
   {
      if (warmupDay < 1)
         warmupDay = 1;

      switch (plan)
      {
      case Gentle:
      {
         // Half of each ceiling, and twice the days: gentle day N is standard
         // day (N + 1) / 2, halved.
         const int ceiling = StandardCeiling((warmupDay + 1) / 2);
         return ceiling < 0 ? -1 : ceiling / 2;
      }
      case Fast:
      {
         // Twice each ceiling, and half the days: fast day N is standard day
         // 2N - 1, doubled. Past day 15 that is past standard day 30.
         if (warmupDay > 15)
            return -1;

         const int ceiling = StandardCeiling(2 * warmupDay - 1);
         return ceiling < 0 ? -1 : ceiling * 2;
      }
      default:
         return StandardCeiling(warmupDay);
      }
   }

   int
   IpWarmup::Ceiling(int plan, int warmupDay, OutboundProviders::Provider provider)
   {
      const int base = BaseCeiling(plan, warmupDay);

      if (base < 0)
         return -1;

      // Every provider outside the five named ones together: five times the
      // ceiling, since what they share is no one reputation system.
      if (provider == OutboundProviders::Other)
         return base * 5;

      return base;
   }

   int
   IpWarmup::LastDay(int plan)
   {
      switch (plan)
      {
      case Gentle:
         return 60;
      case Fast:
         return 15;
      default:
         return 30;
      }
   }

   String
   IpWarmup::PlanName(int plan)
   {
      switch (plan)
      {
      case Gentle:
         return _T("gentle");
      case Fast:
         return _T("fast");
      default:
         return _T("standard");
      }
   }

   bool
   IpWarmup::PlanFromName(const String &name, int &plan)
   {
      for (int candidate = 0; candidate < PlanCount; candidate++)
      {
         if (name.CompareNoCase(PlanName(candidate)) == 0)
         {
            plan = candidate;
            return true;
         }
      }

      return false;
   }

   __int64
   IpWarmup::DayFromDate(int year, int month, int day)
   {
      // Days since 1970-01-01 of a proleptic Gregorian date: the arithmetic of
      // Howard Hinnant's days_from_civil, in signed integers throughout.
      const int y = year - (month <= 2 ? 1 : 0);
      const __int64 era = (y >= 0 ? y : y - 399) / 400;
      const int yearOfEra = (int) (y - era * 400);
      const int dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
      const int dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;

      return era * 146097 + (__int64) dayOfEra - 719468;
   }

   String
   IpWarmup::FormatDate(__int64 dayNumber)
   {
      // civil_from_days, the inverse of the above.
      const __int64 z = dayNumber + 719468;
      const __int64 era = (z >= 0 ? z : z - 146096) / 146097;
      const int dayOfEra = (int) (z - era * 146097);
      const int yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
      const int dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
      const int monthIndex = (5 * dayOfYear + 2) / 153;
      const int day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
      const int month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
      const int year = (int) ((__int64) yearOfEra + era * 400 + (month <= 2 ? 1 : 0));

      String text;
      text.Format(_T("%04d-%02d-%02d"), year, month, day);
      return text;
   }

   bool
   IpWarmup::ParseDate(const String &text, __int64 &dayNumber)
   {
      String value = text;
      value.Trim();

      if (value.GetLength() != 10 || value[4] != '-' || value[7] != '-')
         return false;

      for (int i = 0; i < 10; i++)
      {
         if (i == 4 || i == 7)
            continue;

         if (value[i] < '0' || value[i] > '9')
            return false;
      }

      const int year = _ttoi(value.Mid(0, 4).c_str());
      const int month = _ttoi(value.Mid(5, 2).c_str());
      const int day = _ttoi(value.Mid(8, 2).c_str());

      if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31)
         return false;

      const __int64 candidate = DayFromDate(year, month, day);

      // A date that does not exist - 31 April, 29 February of a common year -
      // comes back as another; refused rather than moved.
      if (FormatDate(candidate) != value)
         return false;

      dayNumber = candidate;
      return true;
   }

   bool
   IpWarmup::CanonicalAddress(const String &text, String &canonical)
   {
      String value = text;
      value.Trim();

      if (value.IsEmpty() || value.GetLength() > AddressMaximum)
         return false;

      IPAddress address;
      if (!address.TryParse(AnsiString(value), false))
         return false;

      // 0.0.0.0 and :: are what "no bind" looks like; a schedule for one would
      // never be consulted.
      if (address.IsAny())
         return false;

      const String written = String(address.ToString());

      if (written.IsEmpty() || written.GetLength() > AddressMaximum)
         return false;

      canonical = written;
      return true;
   }

   // ---- the delivery's side -----------------------------------------------

   bool
   IpWarmup::LoadSchedules_()
   {
      if (schedules_loaded_)
         return true;

      SQLCommand command("select warmupid, warmupaddress, warmupstarted, warmupplan, warmupenabled from hm_warmup");

      std::vector<Schedule> schedules;
      if (!Select_(command, schedules))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7210, "IpWarmup::LoadSchedules_",
            "The IP warm-up schedules could not be read. Deliveries are made as though there were none until they can be.");
         return false;
      }

      schedules_ = schedules;
      schedules_loaded_ = true;
      return true;
   }

   bool
   IpWarmup::FindEnabled(const String &address, Schedule &schedule)
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      if (!LoadSchedules_())
         return false;

      for (const Schedule &candidate : schedules_)
      {
         if (candidate.enabled && candidate.address.CompareNoCase(address) == 0)
         {
            schedule = candidate;
            return true;
         }
      }

      return false;
   }

   IpWarmup::Entry *
   IpWarmup::Find_(__int64 scheduleId, __int64 day, OutboundProviders::Provider provider, bool create, int ceiling, int planDay)
   {
      const CountKey key(scheduleId, day, (int) provider);

      std::map<CountKey, Entry>::iterator existing = counts_.find(key);
      if (existing != counts_.end())
         return &existing->second;

      // A day this far behind will not be written again: its entries go, so
      // the cache holds two days at most.
      for (std::map<CountKey, Entry>::iterator it = counts_.begin(); it != counts_.end(); )
      {
         if (std::get<1>(it->first) < day - 1)
            it = counts_.erase(it);
         else
            ++it;
      }

      const String providerKey = OutboundProviders::Key(provider);

      SQLCommand command("select wcid, wcwarmupid, wcday, wcprovider, wcsent, wcdeferred, wcceiling, wcplanday from hm_warmupcounts where wcwarmupid = @WARMUPID and wcday = @DAY and wcprovider = @PROVIDER");
      command.AddParameter("@WARMUPID", scheduleId);
      command.AddParameter("@DAY", day);
      command.AddParameter("@PROVIDER", providerKey);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7211, "IpWarmup::Find_",
            "A day's IP warm-up count could not be read. The delivery is made as though the address had no schedule.");
         return nullptr;
      }

      Entry entry;

      if (!recordset->IsEOF())
      {
         entry.row_id = recordset->GetInt64Value("wcid");
         entry.sent = (int) recordset->GetLongValue("wcsent");
         entry.deferred = (int) recordset->GetLongValue("wcdeferred");
         entry.ceiling = (int) recordset->GetLongValue("wcceiling");
         entry.plan_day = (int) recordset->GetLongValue("wcplanday");
      }
      else
      {
         if (!create)
            return nullptr;

         SQLStatement statement(SQLStatement::STInsert, "hm_warmupcounts");
         statement.AddColumnInt64("wcwarmupid", scheduleId);
         statement.AddColumnInt64("wcday", day);
         statement.AddColumn("wcprovider", providerKey);
         statement.AddColumn("wcsent", (long) 0);
         statement.AddColumn("wcdeferred", (long) 0);
         statement.AddColumn("wcceiling", (long) ceiling);
         statement.AddColumn("wcplanday", (long) planDay);
         statement.SetIdentityColumn("wcid");

         __int64 id = 0;
         if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id <= 0)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7212, "IpWarmup::Find_",
               "A day's IP warm-up count could not be written. The delivery is made as though the address had no schedule.");
            return nullptr;
         }

         entry.row_id = id;
         entry.ceiling = ceiling;
         entry.plan_day = planDay;
      }

      counts_[key] = entry;
      return &counts_[key];
   }

   bool
   IpWarmup::Write_(const Entry &entry)
   {
      // The whole of the numbers, under the lock every change is made under,
      // so the row is the cache's in the order the changes were made. What
      // deliveries in flight hold is not written: the row says what was sent.
      SQLCommand command("update hm_warmupcounts set wcsent = @SENT, wcdeferred = @DEFERRED, wcceiling = @CEILING, wcplanday = @PLANDAY where wcid = @ID");
      command.AddParameter("@SENT", entry.sent);
      command.AddParameter("@DEFERRED", entry.deferred);
      command.AddParameter("@CEILING", entry.ceiling);
      command.AddParameter("@PLANDAY", entry.plan_day);
      command.AddParameter("@ID", entry.row_id);

      if (!Application::Instance()->GetDBManager()->Execute(command))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7213, "IpWarmup::Write_",
            "A day's IP warm-up count could not be saved. The count held in memory is used, and written whole with the next change to it, until the service restarts or is reinitialised.");
         return false;
      }

      return true;
   }

   int
   IpWarmup::Reserve(__int64 scheduleId, __int64 day, OutboundProviders::Provider provider, int wanted, int ceiling, int planDay,
                     int &sentBefore, bool &heldByOthers)
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      sentBefore = 0;
      heldByOthers = false;

      Entry *entry = Find_(scheduleId, day, provider, true, ceiling, planDay);
      if (!entry)
         return -1;

      // The ceiling and the day in force are the row's, for the history: a
      // plan or a start changed today changes them from now.
      if (entry->ceiling != ceiling || entry->plan_day != planDay)
      {
         entry->ceiling = ceiling;
         entry->plan_day = planDay;
         Write_(*entry);
      }

      sentBefore = entry->sent;

      if (wanted <= 0 || entry->sent >= ceiling)
         return 0;

      // What deliveries in flight hold is room the day may yet get back: when
      // it is part of what does not fit, the caller waits minutes, not the day.
      const int room = ceiling - entry->sent - entry->reserved;

      if (room < wanted)
         heldByOthers = entry->reserved > 0;

      if (room <= 0)
         return 0;

      const int reserved = wanted < room ? wanted : room;
      entry->reserved += reserved;

      return reserved;
   }

   void
   IpWarmup::Settle(__int64 scheduleId, __int64 day, OutboundProviders::Provider provider, int reserved, int delivered)
   {
      if (reserved <= 0 && delivered <= 0)
         return;

      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      // Not made when it is not there: a schedule deleted while its delivery
      // was in flight has no row to settle. A cache forgotten meanwhile is read
      // again, with nothing held in flight, which the floor below keeps at 0.
      Entry *entry = Find_(scheduleId, day, provider, false, 0, 0);
      if (!entry)
         return;

      entry->reserved = entry->reserved > reserved ? entry->reserved - reserved : 0;

      if (delivered > 0)
      {
         entry->sent += delivered;
         Write_(*entry);
      }
   }

   void
   IpWarmup::AddDeferred(__int64 scheduleId, __int64 day, OutboundProviders::Provider provider, int count)
   {
      if (count <= 0)
         return;

      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      // The row exists: Reserve made it in the same pass.
      Entry *entry = Find_(scheduleId, day, provider, false, 0, 0);
      if (!entry)
         return;

      entry->deferred += count;
      Write_(*entry);
   }

   void
   IpWarmup::Forget()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      schedules_loaded_ = false;
      schedules_.clear();
      counts_.clear();
   }

   void
   IpWarmup::ForgetSchedules()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      schedules_loaded_ = false;
      schedules_.clear();
   }

   // ---- the administrator's side ------------------------------------------

   bool
   IpWarmup::Select_(SQLCommand &command, std::vector<Schedule> &schedules)
   {
      schedules.clear();

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         Schedule schedule;
         schedule.id = recordset->GetInt64Value("warmupid");
         schedule.address = recordset->GetStringValue("warmupaddress");
         schedule.started = recordset->GetInt64Value("warmupstarted");
         schedule.plan = (int) recordset->GetLongValue("warmupplan");
         schedule.enabled = recordset->GetLongValue("warmupenabled") != 0;

         if (schedule.plan < 0 || schedule.plan >= PlanCount)
            schedule.plan = Standard;

         schedules.push_back(schedule);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   IpWarmup::SelectCounts_(SQLCommand &command, std::vector<Count> &counts)
   {
      counts.clear();

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         Count count;
         count.schedule_id = recordset->GetInt64Value("wcwarmupid");
         count.day = recordset->GetInt64Value("wcday");
         count.provider = recordset->GetStringValue("wcprovider");
         count.sent = (int) recordset->GetLongValue("wcsent");
         count.deferred = (int) recordset->GetLongValue("wcdeferred");
         count.ceiling = (int) recordset->GetLongValue("wcceiling");
         count.plan_day = (int) recordset->GetLongValue("wcplanday");

         counts.push_back(count);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   IpWarmup::ReadAll(std::vector<Schedule> &schedules)
   {
      SQLCommand command("select warmupid, warmupaddress, warmupstarted, warmupplan, warmupenabled from hm_warmup order by warmupid");
      return Select_(command, schedules);
   }

   bool
   IpWarmup::Read(__int64 id, Schedule &schedule, bool &found)
   {
      found = false;

      SQLCommand command("select warmupid, warmupaddress, warmupstarted, warmupplan, warmupenabled from hm_warmup where warmupid = @WARMUPID");
      command.AddParameter("@WARMUPID", id);

      std::vector<Schedule> schedules;
      if (!Select_(command, schedules))
         return false;

      if (!schedules.empty())
      {
         schedule = schedules[0];
         found = true;
      }

      return true;
   }

   bool
   IpWarmup::AddressTaken(const String &address, __int64 exceptId, bool &taken)
   {
      taken = false;

      SQLCommand command("select warmupid, warmupaddress, warmupstarted, warmupplan, warmupenabled from hm_warmup where warmupaddress = @ADDRESS");
      command.AddParameter("@ADDRESS", address);

      std::vector<Schedule> schedules;
      if (!Select_(command, schedules))
         return false;

      for (const Schedule &schedule : schedules)
      {
         if (schedule.id != exceptId)
            taken = true;
      }

      return true;
   }

   bool
   IpWarmup::Save(Schedule &schedule)
   {
      const bool isNew = schedule.id == 0;

      SQLStatement statement(isNew ? SQLStatement::STInsert : SQLStatement::STUpdate, "hm_warmup");
      statement.AddColumn("warmupaddress", schedule.address);
      statement.AddColumnInt64("warmupstarted", schedule.started);
      statement.AddColumn("warmupplan", (long) schedule.plan);
      statement.AddColumn("warmupenabled", (long) (schedule.enabled ? 1 : 0));

      if (isNew)
      {
         statement.SetIdentityColumn("warmupid");
      }
      else
      {
         String where;
         where.Format(_T("warmupid = %I64d"), schedule.id);
         statement.SetWhereClause(where);
      }

      __int64 id = 0;
      const bool saved = Application::Instance()->GetDBManager()->Execute(statement, isNew ? &id : 0);

      if (saved && isNew)
         schedule.id = id;

      ForgetSchedules();

      return saved && schedule.id > 0;
   }

   bool
   IpWarmup::Delete(__int64 id, bool &found)
   {
      found = false;

      Schedule existing;
      if (!Read(id, existing, found))
         return false;

      if (!found)
         return true;

      // The counts first, though the foreign key would take them: the order
      // is the one every backend accepts whatever its cascade does.
      SQLCommand counts("delete from hm_warmupcounts where wcwarmupid = @WARMUPID");
      counts.AddParameter("@WARMUPID", id);

      SQLCommand schedule("delete from hm_warmup where warmupid = @WARMUPID");
      schedule.AddParameter("@WARMUPID", id);

      const bool deleted = Application::Instance()->GetDBManager()->Execute(counts) &&
                           Application::Instance()->GetDBManager()->Execute(schedule);

      ForgetSchedules();

      return deleted;
   }

   bool
   IpWarmup::CountsOn(__int64 day, std::vector<Count> &counts)
   {
      SQLCommand command("select wcid, wcwarmupid, wcday, wcprovider, wcsent, wcdeferred, wcceiling, wcplanday from hm_warmupcounts where wcday = @DAY");
      command.AddParameter("@DAY", day);
      return SelectCounts_(command, counts);
   }

   bool
   IpWarmup::History(__int64 scheduleId, std::vector<Count> &counts)
   {
      SQLCommand command("select wcid, wcwarmupid, wcday, wcprovider, wcsent, wcdeferred, wcceiling, wcplanday from hm_warmupcounts where wcwarmupid = @WARMUPID order by wcday desc, wcprovider asc");
      command.AddParameter("@WARMUPID", scheduleId);
      return SelectCounts_(command, counts);
   }

   // ---- the configuration backup ------------------------------------------

   bool
   IpWarmup::XMLStore(XNode *parentNode)
   {
      std::vector<Schedule> schedules;
      if (!ReadAll(schedules))
         return false;

      if (schedules.empty())
         return true;

      XNode *collection = parentNode->AppendChild(_T("WarmupSchedules"));

      for (const Schedule &schedule : schedules)
      {
         XNode *node = collection->AppendChild(_T("WarmupSchedule"));
         node->AppendAttr(_T("Address"), schedule.address);
         node->AppendAttr(_T("Started"), FormatDate(schedule.started));
         node->AppendAttr(_T("Plan"), PlanName(schedule.plan));
         node->AppendAttr(_T("Enabled"), schedule.enabled ? _T("1") : _T("0"));
      }

      return true;
   }

   bool
   IpWarmup::XMLLoad(XNode *parentNode)
   {
      std::vector<Schedule> existing;
      if (!ReadAll(existing))
         return false;

      std::vector<Schedule> wanted;

      XNode *collection = parentNode->GetChild(_T("WarmupSchedules"));
      if (collection)
      {
         for (int i = 0; i < collection->GetChildCount(); i++)
         {
            XNode *node = collection->GetChild(i);

            Schedule schedule;
            __int64 started = 0;

            // A schedule the archive holds wrongly - an address or a date this
            // build would refuse - is left out and said, rather than stored.
            if (!CanonicalAddress(node->GetAttrValue(_T("Address")), schedule.address) ||
                !ParseDate(node->GetAttrValue(_T("Started")), started))
            {
               LOG_APPLICATION("IP warm-up: a schedule in the backup names an address or a start this server cannot read, and is not restored.");
               continue;
            }

            schedule.started = started;

            int plan = Standard;
            PlanFromName(node->GetAttrValue(_T("Plan")), plan);
            schedule.plan = plan;
            schedule.enabled = node->GetAttrValue(_T("Enabled")) != _T("0");

            wanted.push_back(schedule);
         }
      }

      // One here that the backup names keeps its row, and so its counts.
      for (Schedule &schedule : wanted)
      {
         for (const Schedule &current : existing)
         {
            if (current.address.CompareNoCase(schedule.address) == 0)
               schedule.id = current.id;
         }
      }

      for (const Schedule &current : existing)
      {
         bool kept = false;
         for (const Schedule &schedule : wanted)
         {
            if (schedule.id == current.id)
               kept = true;
         }

         bool found = false;
         if (!kept && !Delete(current.id, found))
            return false;
      }

      for (Schedule &schedule : wanted)
      {
         if (!Save(schedule))
            return false;
      }

      ForgetSchedules();

      return true;
   }
}
