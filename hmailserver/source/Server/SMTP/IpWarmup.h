// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The IP warm-up: a schedule for a new outbound address.
//
// A large provider judges a sending address by its history, and an address
// with none that starts sending thousands of messages a day is treated as a
// spammer's would be - deferred, then filtered, then refused. The cure every
// sending service applies is a warm-up: so many messages a day to each large
// provider on the first day, a little more on each day after, over some weeks,
// until the address has a history and the ceilings go. This is that, enforced
// by the queue (ExternalDelivery::DeliverToSingleDomain_) rather than by
// whoever is sending.
//
// A schedule is a row of hm_warmup (schema 6093): the outbound address it is
// about - the local IPv4 or IPv6 address a delivery binds, as
// ExternalDelivery::GetLocalAddress_ chooses it from a rule's bind-to-address
// action or SMTPDeliveryBindToIP - the UTC day it began, the plan it follows
// and whether it is enabled. The plans are fixed tables of a daily ceiling per
// provider by the day of the warm-up (Ceiling, and docs/IpWarmup.md prints
// them): standard, over thirty days; gentle, half of each ceiling and twice
// the days; fast, twice each and half the days. A provider outside the five
// named ones (SMTP/OutboundProviders.h) gets five times the ceiling. After the
// plan's last day there is no ceiling and the schedule does nothing more.
//
// What is counted is RECIPIENTS, per schedule, per UTC day, per provider, in
// hm_warmupcounts: what a provider's reputation counts is the recipients it
// was offered, not the messages. A delivery reserves its batch's recipients
// before it connects, up to the ceiling, and settles when the attempts are
// over: what was delivered is added to the day's sent, and the rest of the
// reservation is let go. A reservation in flight is held in memory only, apart
// from sent, so that the table and the routes say what was delivered and a
// delivery that finds the room held by others in flight - which may yet give
// it back - is told so, and waits minutes rather than the day. Sent and
// deferred are written through to the table at every change, under one lock,
// with the ceiling and the warm-up day in force, so a restart resumes the day
// where it was and the history says what was in force then. The cache in
// front of them exists so that a delivery does not read the table: the
// schedules are forgotten whenever one is written, and the counts at
// Application::Reinitialize.
//
// Per address, not per pool: a schedule is about exactly one address. A batch
// is judged against the address it will bind for its first host and the
// provider that host belongs to, and a later host that would be reached from
// another scheduled address is not tried for it.
//
// The schedules - not the counts, which are traffic - are carried in the
// configuration backup beside the remote domain policies (XMLStore, XMLLoad).

#pragma once

#include "../Common/Util/Singleton.h"
#include "OutboundProviders.h"

#include <map>
#include <tuple>
#include <vector>

namespace HM
{
   class SQLCommand;

   class IpWarmup : public Singleton<IpWarmup>
   {
   public:
      enum Plan
      {
         Standard = 0,
         Gentle = 1,
         Fast = 2
      };

      // How many plans there are: a plan is 0 to PlanCount - 1.
      static const int PlanCount = 3;

      // A UTC day is this many seconds: day numbers are seconds since the
      // epoch divided by it, so day 0 is 1 January 1970.
      static const int DaySeconds = 86400;

      struct Schedule
      {
         Schedule() : id(0), started(0), plan(Standard), enabled(true) {}

         __int64 id;
         String address;      // canonical, as IPAddress::ToString writes it
         __int64 started;     // the UTC day number of the schedule's first day
         int plan;            // a Plan
         bool enabled;
      };

      struct Count
      {
         Count() : schedule_id(0), day(0), sent(0), deferred(0), ceiling(0), plan_day(0) {}

         __int64 schedule_id;
         __int64 day;         // the UTC day number
         String provider;     // an OutboundProviders key
         int sent;            // recipients delivered that day
         int deferred;        // recipients held over the ceiling that day, per pass
         int ceiling;         // the ceiling in force when the row was last written
         int plan_day;        // the warm-up day in force then
      };

      IpWarmup();
      virtual ~IpWarmup();

      // ---- the arithmetic, pure ------------------------------------------

      // The UTC day number of a moment in seconds since the epoch.
      static __int64 DayOf(__int64 moment);

      // Which day of its warm-up a schedule begun on started is on, today:
      // 1 on the day it began. A start later than today is day 1 - the most
      // careful answer, and the REST routes refuse one anyway.
      static int WarmupDay(__int64 started, __int64 today);

      // The daily ceiling of a plan on a warm-up day for one of the five named
      // providers, or -1 when there is none (the plan is over).
      static int BaseCeiling(int plan, int warmupDay);

      // The daily ceiling for a provider: the base for the five named ones,
      // five times it for the rest; -1 when there is none.
      static int Ceiling(int plan, int warmupDay, OutboundProviders::Provider provider);

      // The last warm-up day of a plan that has a ceiling: 30, 60 or 15.
      static int LastDay(int plan);

      // standard, gentle, fast.
      static String PlanName(int plan);
      static bool PlanFromName(const String &name, int &plan);

      // The day number of a civil date, and back; ParseDate reads exactly
      // YYYY-MM-DD and refuses a date that does not exist.
      static __int64 DayFromDate(int year, int month, int day);
      static bool ParseDate(const String &text, __int64 &dayNumber);
      static String FormatDate(__int64 dayNumber);

      // The address as a delivery will name it (IPAddress::ToString), or false
      // for one that is not an IPv4 or IPv6 address, is the unspecified
      // address, or is longer than the column holds.
      static bool CanonicalAddress(const String &text, String &canonical);

      // ---- the delivery's side -------------------------------------------

      // The enabled schedule for an address, when there is one. The
      // schedules are read once and cached until they are written.
      bool FindEnabled(const String &address, Schedule &schedule);

      // Reserves up to wanted recipients of a day's room for a provider: the
      // ceiling, less what was sent, less what deliveries still in flight hold.
      // Answers how many were reserved - 0 when there is no room - or -1 when
      // the count could not be read or started (the error is reported and the
      // caller delivers as though there were no schedule). sentBefore is what
      // the day had sent. heldByOthers is true when some of what did not fit
      // is room other deliveries in flight hold and may give back; false when
      // the day's ceiling itself is what was reached. The ceiling and the
      // warm-up day are stored on the day's row.
      int Reserve(__int64 scheduleId, __int64 day, OutboundProviders::Provider provider, int wanted, int ceiling, int planDay,
                  int &sentBefore, bool &heldByOthers);

      // Ends a reservation: delivered is added to the day's sent, and the
      // whole reservation is let go.
      void Settle(__int64 scheduleId, __int64 day, OutboundProviders::Provider provider, int reserved, int delivered);

      // Counts recipients held over the ceiling.
      void AddDeferred(__int64 scheduleId, __int64 day, OutboundProviders::Provider provider, int count);

      // Forgets the cached schedules and counts: read again at the next use.
      // For Application::Reinitialize, after which the tables may hold
      // anything; a reservation in flight is let go by its Settle whatever
      // this has forgotten.
      void Forget();

      // Forgets the cached schedules only, for a schedule written: the counts
      // are keyed by schedule and stay right.
      void ForgetSchedules();

      // ---- the administrator's side (RestApiWarmup.cpp) ------------------

      bool ReadAll(std::vector<Schedule> &schedules);
      bool Read(__int64 id, Schedule &schedule, bool &found);

      // Whether another schedule (not exceptId) names the address.
      bool AddressTaken(const String &address, __int64 exceptId, bool &taken);

      // Inserts a schedule whose id is 0 and fills the id; updates one whose
      // id is not. Forgets the cached schedules either way.
      bool Save(Schedule &schedule);

      // Removes a schedule and its counts. Forgets the cached schedules.
      bool Delete(__int64 id, bool &found);

      // Every schedule's counts on one day, and one schedule's every day,
      // newest first.
      bool CountsOn(__int64 day, std::vector<Count> &counts);
      bool History(__int64 scheduleId, std::vector<Count> &counts);

      // ---- the configuration backup (SMTPConfiguration::XMLStore) --------

      // The schedules as <WarmupSchedules><WarmupSchedule Address Started
      // Plan Enabled/>...; nothing when there are none. The counts are traffic
      // and are not carried.
      bool XMLStore(XNode *parentNode);

      // The schedules the backup holds, matched to the ones here by address:
      // one here that the backup names is changed to the backup's and keeps
      // its counts, one the backup does not name is removed with its counts,
      // and one the backup adds starts with none.
      bool XMLLoad(XNode *parentNode);

   private:

      struct Entry
      {
         Entry() : row_id(0), sent(0), deferred(0), ceiling(0), plan_day(0), reserved(0) {}

         __int64 row_id;
         int sent;
         int deferred;
         int ceiling;
         int plan_day;
         int reserved;        // held by deliveries in flight; in memory only
      };

      typedef std::tuple<__int64, __int64, int> CountKey;   // schedule, day, provider

      bool LoadSchedules_();
      Entry *Find_(__int64 scheduleId, __int64 day, OutboundProviders::Provider provider, bool create, int ceiling, int planDay);
      bool Write_(const Entry &entry);

      static bool Select_(SQLCommand &command, std::vector<Schedule> &schedules);
      static bool SelectCounts_(SQLCommand &command, std::vector<Count> &counts);

      boost::recursive_mutex mutex_;
      bool schedules_loaded_;
      std::vector<Schedule> schedules_;
      std::map<CountKey, Entry> counts_;
   };
}
