// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See ResourceBooking.h.

#include "StdAfx.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

#include "ResourceBooking.h"
#include "Imip.h"
#include "ICalendar.h"
#include "CalendarStore.h"
#include "DavSupport.h"
#include "FileUtilities.h"
#include "GUIDCreator.h"
#include "Time.h"
#include "Unicode.h"
#include "RestrictedDelivery.h"
#include "Encoding/Base64.h"
#include "../BO/Account.h"
#include "../BO/DomainAliases.h"
#include "../BO/Message.h"
#include "../BO/MessageRecipients.h"
#include "../Application/ObjectCache.h"
#include "../Application/Application.h"
#include "../Cache/CacheContainer.h"
#include "../Mime/Mime.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DALRecordset.h"
#include "../Threading/WorkQueue.h"
#include "../Threading/WorkQueueManager.h"
#include "../Threading/Task.h"
#include "../AntiSpam/AuthenticationResults.h"
#include "../../SMTP/RecipientParser.h"
#include "../../SMTP/SMTPConnection.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const int ResourceBooking::MaximumWindowDays;
   const int ResourceBooking::MaximumDurationMinutes;
   const int ResourceBooking::MaximumBookersLength;
   const int ResourceBooking::MaximumCapacity;
   const int ResourceBooking::MaximumTimeZoneLength;
   const int ResourceBooking::AnswersPerHour;
   const int ResourceBooking::NoticesPerHour;
   const int ResourceBooking::AllNoticesPerHour;
   const int ResourceBooking::MaximumAttempts;
   const int ResourceBooking::DrainThreads;

   namespace
   {
      const char *PolicyColumns =
         "bpaccountid, bpautoaccept, bpwindowdays, bpmaxminutes, bpworkinghoursonly, bpworkdays, bpworkstart, "
         "bpworkend, bptimezone, bpallowconflicts, bpallowrecurring, bpbookers, bpcapacity";

      // How many of a declined booking's conflicts the answer names; the rest
      // are counted.
      const size_t ConflictsNamed = 3;

      // The answer memory's hour.
      const __int64 MemoryHour = 3600;

      AnsiString Utf8Of(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      String WideOf(const AnsiString &value)
      {
         String out;
         Unicode::MultiByteToWide(value, out);
         return out;
      }

      AnsiString Trimmed(const AnsiString &value)
      {
         AnsiString trimmed = value;
         trimmed.TrimLeft();
         trimmed.TrimRight();
         return trimmed;
      }

      String TrimmedWide(const String &value)
      {
         String trimmed = value;
         trimmed.TrimLeft();
         trimmed.TrimRight();
         return trimmed;
      }

      String LowerWide(const String &value)
      {
         String lower = TrimmedWide(value);
         lower.ToLower();
         return lower;
      }

      AnsiString TwoDigits(int value)
      {
         AnsiString text;
         text.Format("%02d", value);
         return text;
      }

      AnsiString IntText(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      // An address as this server resolves it: lower-cased, an alias domain's
      // spelling turned into its domain's, as RecipientParser and the logon do.
      AnsiString Normalised(const String &address)
      {
         String trimmed = TrimmedWide(address);
         if (trimmed.IsEmpty())
            return "";
         std::shared_ptr<DomainAliases> domainAliases = ObjectCache::Instance()->GetDomainAliases();
         String primary = domainAliases ? domainAliases->ApplyAliasesOnAddress(trimmed) : trimmed;
         primary.ToLower();
         return Utf8Of(primary);
      }

      // The domain of an address, or "".
      AnsiString DomainPart(const AnsiString &address)
      {
         const int at = address.ReverseFind('@');
         return at >= 0 ? address.Mid(at + 1) : AnsiString();
      }

      // A domain as this server resolves it: an alias domain turned into its
      // domain, lower-cased.
      AnsiString NormalisedDomain(const String &domain)
      {
         const String trimmed = TrimmedWide(domain);
         if (trimmed.IsEmpty())
            return "";
         return DomainPart(Normalised(_T("x@") + trimmed));
      }

      // Whether the address is at one of the server's own domains (an alias
      // domain resolved first). An empty address - the null sender - is not.
      bool IsLocalAddress(const AnsiString &normalised)
      {
         const AnsiString domain = DomainPart(normalised);
         if (domain.IsEmpty() || normalised.Find("@") <= 0)
            return false;
         return CacheContainer::Instance()->GetDomain(WideOf(domain)) != nullptr;
      }

      // Whether the delivered message asks not to be answered automatically:
      // an Auto-Submitted header whose value is anything but "no" (RFC 3834
      // section 5), read from the delivered copy - the one header this unit
      // takes a sender's word for, since believing it can only keep an answer
      // from being sent.
      bool AutoSubmittedIn(const String &fileName)
      {
         AnsiString header = PersistentMessage::LoadHeader(fileName, false);
         if (header.IsEmpty())
            return false;
         MimeHeader mimeHeader;
         mimeHeader.Load(header.c_str(), header.GetLength(), true);
         const char *raw = mimeHeader.GetRawFieldValue("Auto-Submitted");
         if (!raw)
            return false;
         AnsiString value = raw;
         const int semicolon = value.Find(";");
         if (semicolon >= 0)
            value = value.Mid(0, semicolon);
         value = Trimmed(value);
         return !value.IsEmpty() && value.CompareNoCase("no") != 0;
      }

      // The resource as a person reads it: its name, or its address.
      AnsiString DisplayNameOf(const Account &resource)
      {
         String name = resource.GetPersonFirstName();
         const String last = resource.GetPersonLastName();
         if (!last.IsEmpty())
         {
            if (!name.IsEmpty())
               name += _T(" ");
            name += last;
         }
         name = TrimmedWide(name);
         if (name.IsEmpty())
            name = resource.GetAddress();
         return Utf8Of(name);
      }

      // "Name" <address>, or the bare address.
      AnsiString FromHeaderOf(const Account &resource)
      {
         String name = resource.GetPersonFirstName();
         const String last = resource.GetPersonLastName();
         if (!last.IsEmpty())
         {
            if (!name.IsEmpty())
               name += _T(" ");
            name += last;
         }
         name = TrimmedWide(name);
         if (name.IsEmpty())
            return Utf8Of(resource.GetAddress());
         return "\"" + Utf8Of(name) + "\" <" + Utf8Of(resource.GetAddress()) + ">";
      }

      AnsiString HeaderSafe(const AnsiString &text)
      {
         AnsiString out;
         for (int i = 0; i < text.GetLength(); i++)
         {
            const char c = text.c_str()[i];
            if (c == '\r' || c == '\n')
               continue;
            out += c;
         }
         return out;
      }

      bool IsAscii(const AnsiString &text)
      {
         for (int i = 0; i < text.GetLength(); i++)
         {
            if (static_cast<unsigned char>(text.c_str()[i]) > 126)
               return false;
         }
         return true;
      }

      AnsiString EncodedWord(const AnsiString &text)
      {
         if (IsAscii(text))
            return text;
         return "=?utf-8?B?" + Base64::Encode(text.c_str(), text.GetLength()) + "?=";
      }

      AnsiString Base64Lines(const AnsiString &bytes)
      {
         AnsiString encoded = Base64::Encode(bytes.c_str(), bytes.GetLength());
         AnsiString out;
         for (int i = 0; i < encoded.GetLength(); i += 76)
            out += encoded.Mid(i, 76) + "\r\n";
         return out;
      }

      // One lock per resource, recursive so that a delegate's REST write can
      // take it around code that takes it again (ResourceCalendarGuard): the
      // decision about a request and the filing of it are one step under it.
      // The map holds a lock per resource that ever took a request or a
      // write, which is as many locks as there are rooms.
      std::mutex locksGuard;
      std::map<__int64, std::shared_ptr<std::recursive_mutex> > locks;

      std::shared_ptr<std::recursive_mutex> LockOf(__int64 accountId)
      {
         std::lock_guard<std::mutex> guard(locksGuard);
         std::shared_ptr<std::recursive_mutex> &slot = locks[accountId];
         if (!slot)
            slot = std::shared_ptr<std::recursive_mutex>(new std::recursive_mutex());
         return slot;
      }

      // Monday 0 to Sunday 6, of a date in the proleptic Gregorian calendar.
      int DayOfWeek(int year, int month, int day)
      {
         long long y = year;
         const long long m = month;
         const long long d = day;
         if (m <= 2)
            y -= 1;
         const long long era = (y >= 0 ? y : y - 399) / 400;
         const long long yoe = y - era * 400;
         const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
         const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
         const long long days = era * 146097 + doe - 719468;   // 1970-01-01, a Thursday, is 0
         const long long thursdayBased = ((days % 7) + 7) % 7;
         return static_cast<int>((thursdayBased + 3) % 7);
      }

      AnsiString ClockText(int minutes)
      {
         return TwoDigits(minutes / 60) + ":" + TwoDigits(minutes % 60);
      }

      // Whether an occurrence lies inside the working hours: on a working day,
      // starting at or after the start and ending by the end, in the policy's
      // zone. An all-day booking never does, and one that crosses midnight
      // does not either.
      bool InWorkingHours(const BookingPolicy &policy, const AnsiString &zone, const ICalInstance &instance)
      {
         if (instance.allDay)
            return false;

         int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
         if (!ICalendar::InstantToLocal(zone, instance.start, y, m, d, hh, mm, ss))
            return false;

         if ((policy.workDays & (1 << DayOfWeek(y, m, d))) == 0)
            return false;

         const int startMinutes = hh * 60 + mm;
         const __int64 end = instance.end > instance.start ? instance.end : instance.start;

         // The last second of the booking must fall on the same day; a booking
         // that ends at midnight ends at the day's 1440th minute.
         int ly = 0, lm = 0, ld = 0, lhh = 0, lmm = 0, lss = 0;
         if (!ICalendar::InstantToLocal(zone, end > instance.start ? end - 1 : end, ly, lm, ld, lhh, lmm, lss))
            return false;
         if (ly != y || lm != m || ld != d)
            return false;

         int ey = 0, em = 0, ed = 0, ehh = 0, emm = 0, ess = 0;
         if (!ICalendar::InstantToLocal(zone, end, ey, em, ed, ehh, emm, ess))
            return false;
         int endMinutes = (ey == y && em == m && ed == d) ? ehh * 60 + emm + (ess > 0 ? 1 : 0) : 1440;

         return startMinutes >= policy.workStart && endMinutes <= policy.workEnd;
      }

      bool IsEventLike(const ICalComponent &component)
      {
         return component.name == "VEVENT" || component.name == "VTODO";
      }

      // The component without a RECURRENCE-ID, or the first event when every
      // one has one.
      const ICalComponent *MasterOf(const ICalComponent &calendar)
      {
         const ICalComponent *first = nullptr;
         for (size_t i = 0; i < calendar.children.size(); i++)
         {
            const ICalComponent &component = calendar.children[i];
            if (!IsEventLike(component))
               continue;
            if (!component.Property("RECURRENCE-ID"))
               return &component;
            if (!first)
               first = &component;
         }
         return first;
      }

      ICalComponent *MutableMasterOf(ICalComponent &calendar)
      {
         for (size_t i = 0; i < calendar.children.size(); i++)
         {
            ICalComponent &component = calendar.children[i];
            if (IsEventLike(component) && !component.Property("RECURRENCE-ID"))
               return &component;
         }
         return nullptr;
      }

      // Whether the object has a master: a REQUEST without one carries only
      // changed occurrences of a series (RFC 5546 section 3.2.2.1).
      bool HasMaster(const ICalComponent &calendar)
      {
         for (size_t i = 0; i < calendar.children.size(); i++)
         {
            if (IsEventLike(calendar.children[i]) && !calendar.children[i].Property("RECURRENCE-ID"))
               return true;
         }
         return false;
      }

      size_t EventCount(const ICalComponent &calendar)
      {
         size_t count = 0;
         for (size_t i = 0; i < calendar.children.size(); i++)
         {
            if (IsEventLike(calendar.children[i]))
               count++;
         }
         return count;
      }

      int SequenceOf(const ICalComponent &component)
      {
         AnsiString text = Trimmed(component.Value("SEQUENCE"));
         return text.IsEmpty() ? 0 : atoi(text.c_str());
      }

      // The instant a component's RECURRENCE-ID names, read in its calendar.
      bool RecurrenceIdOf(const ICalComponent &component, const ICalComponent &calendar, __int64 &instant)
      {
         const ICalProperty *recurrenceId = component.Property("RECURRENCE-ID");
         if (!recurrenceId)
            return false;
         ICalTime time;
         AnsiString ignored;
         if (!ICalendar::ParseTime(*recurrenceId, &calendar, time, ignored))
            return false;
         instant = time.instant;
         return true;
      }

      // Every override of the occurrence taken out of the calendar.
      void RemoveOverrides(ICalComponent &calendar, __int64 recurrenceId)
      {
         for (size_t i = 0; i < calendar.children.size();)
         {
            __int64 instant = 0;
            if (IsEventLike(calendar.children[i]) && RecurrenceIdOf(calendar.children[i], calendar, instant) && instant == recurrenceId)
            {
               calendar.children.erase(calendar.children.begin() + i);
               continue;
            }
            i++;
         }
      }

      // The source's time zones the target does not carry yet, carried across,
      // so that a component moved from one object to the other is still placed.
      void AdoptTimeZones(ICalComponent &target, const ICalComponent &source)
      {
         for (size_t s = 0; s < source.children.size(); s++)
         {
            if (source.children[s].name != "VTIMEZONE")
               continue;
            const AnsiString tzid = source.children[s].Value("TZID");
            bool present = false;
            for (size_t t = 0; t < target.children.size() && !present; t++)
               present = target.children[t].name == "VTIMEZONE" && target.children[t].Value("TZID") == tzid;
            if (!present)
               target.children.insert(target.children.begin(), source.children[s]);
         }
      }

      // The booking the resource's calendars hold under a UID, and what its
      // master says.
      struct HeldBooking
      {
         HeldBooking() : found(false), sequence(0) { }

         bool found;
         CalendarRecord calendar;
         CalendarObjectRecord record;
         ICalComponent tree;
         AnsiString organiser;   // normalised
         int sequence;
      };

      // False when the calendars could not be read; held.found says whether
      // one of them holds the UID.
      bool FindHeld(__int64 accountId, const String &uid, HeldBooking &held)
      {
         held = HeldBooking();

         std::vector<CalendarRecord> calendars;
         if (!CalendarStore::ListCalendarsIfAny(accountId, calendars))
            return false;

         for (size_t c = 0; c < calendars.size(); c++)
         {
            CalendarObjectRecord record;
            if (!CalendarStore::FindByUid(accountId, calendars[c].id, uid, record))
               continue;

            held.found = true;
            held.calendar = calendars[c];
            held.record = record;

            AnsiString ignored;
            if (ICalendar::Parse(Utf8Of(record.data), held.tree, ignored))
            {
               if (const ICalComponent *master = MasterOf(held.tree))
               {
                  held.organiser = Normalised(WideOf(Imip::AddressOf(master->Value("ORGANIZER"))));
                  held.sequence = SequenceOf(*master);
               }
            }
            return true;
         }

         return true;
      }

      // The stored object replaced by the tree, as the delivery filer writes
      // one: parsed again, summarised, stamped.
      bool WriteTree(__int64 accountId, const HeldBooking &held, const ICalComponent &tree, AnsiString &problem)
      {
         const AnsiString serialized = ICalendar::Serialize(tree);
         ICalComponent parsed;
         if (!ICalendar::Parse(serialized, parsed, problem))
            return false;
         ICalendar::Summary summary;
         if (!ICalendar::Summarize(parsed, summary, problem))
            return false;

         CalendarObjectRecord record = held.record;
         record.data = WideOf(serialized);
         record.etag = Dav::Wide(Dav::EtagOf(serialized));
         record.start = summary.start;
         record.end = summary.end;
         record.first = summary.first;
         record.last = summary.last;

         __int64 token = 0;
         if (!CalendarStore::Update(accountId, held.calendar.id, held.record.id, record, token))
         {
            problem = "the calendar could not be written";
            return false;
         }
         return true;
      }

      // A group of this server by its name, compared without case: its id,
      // and its name as the server has it; 0 when there is none.
      __int64 GroupIdOf(const String &name, String &canonical)
      {
         canonical = _T("");
         SQLCommand command("select groupid, groupname from hm_groups where lower(groupname) = @NAME");
         command.AddParameter("@NAME", LowerWide(name));
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset || recordset->IsEOF())
            return 0;
         canonical = recordset->GetStringValue("groupname");
         return recordset->GetInt64Value("groupid");
      }

      bool IsGroupMember(const String &groupName, __int64 accountId)
      {
         SQLCommand command("select count(*) as c from hm_group_members inner join hm_groups on groupid = membergroupid "
                            "where lower(groupname) = @NAME and memberaccountid = @ACCOUNTID");
         command.AddParameter("@NAME", LowerWide(groupName));
         command.AddParameter("@ACCOUNTID", accountId);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         return recordset && !recordset->IsEOF() && recordset->GetLongValue("c") > 0;
      }

      bool IsListMember(const String &listAddress, const String &address)
      {
         SQLCommand command("select count(*) as c from hm_distributionlistsrecipients inner join hm_distributionlists "
                            "on distributionlistid = distributionlistrecipientlistid "
                            "where lower(distributionlistaddress) = @LIST and lower(distributionlistrecipientaddress) = @ADDRESS");
         command.AddParameter("@LIST", listAddress);
         command.AddParameter("@ADDRESS", address);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         return recordset && !recordset->IsEOF() && recordset->GetLongValue("c") > 0;
      }

      // Whether the policy names the organiser itself, or its @domain - the
      // only naming an organiser from outside can have, since a distribution
      // list or a group of this server holds this server's own people. Alias
      // domains are resolved on both sides.
      bool NamedDirectly(const BookingPolicy &policy, const AnsiString &organiser)
      {
         const AnsiString organiserDomain = DomainPart(organiser);
         const std::vector<String> entries = ResourceBooking::SplitList(policy.bookers);
         for (size_t i = 0; i < entries.size(); i++)
         {
            const String entry = LowerWide(entries[i]);
            if (entry.StartsWith(_T("@")))
            {
               if (!organiserDomain.IsEmpty() && NormalisedDomain(entry.Mid(1)) == organiserDomain)
                  return true;
               continue;
            }
            if (entry.Find(_T("@")) > 0 && Normalised(entry) == organiser)
               return true;
         }
         return false;
      }

      // Whether the policy lets the organiser book. An empty list is everyone
      // in the server's own domains; a list is its entries alone - an
      // address, an address that is a distribution list of this server (its
      // members), @domain (anyone there), or a group's name (its members).
      bool MayBook(const BookingPolicy &policy, const AnsiString &organiser)
      {
         const std::vector<String> entries = ResourceBooking::SplitList(policy.bookers);
         if (entries.empty())
            return IsLocalAddress(organiser);

         if (NamedDirectly(policy, organiser))
            return true;

         const String organiserWide = WideOf(organiser);
         std::shared_ptr<const Account> organiserAccount = CacheContainer::Instance()->GetAccount(organiserWide);

         for (size_t i = 0; i < entries.size(); i++)
         {
            const String entry = LowerWide(entries[i]);
            if (entry.StartsWith(_T("@")))
               continue;

            if (entry.Find(_T("@")) > 0)
            {
               if (IsListMember(WideOf(Normalised(entry)), organiserWide))
                  return true;
               continue;
            }

            if (organiserAccount && IsGroupMember(entries[i], organiserAccount->GetID()))
               return true;
         }

         return false;
      }

      // Whether the message's own record says DMARC passed for the domain -
      // the From's, which DMARC holds an aligned SPF or DKIM pass to. SPF on its
      // own is not enough and is not recorded: it vouches for the connecting
      // address for an envelope domain whatever the From says, and a shared
      // platform named in a domain's SPF record carries other tenants' mail.
      // The record is kept for the job only when the From was the organiser
      // (OnDelivery), so a pass here is a pass for the organiser's own From.
      bool Vouched(const AnsiString &authDomains, const AnsiString &domain)
      {
         if (domain.IsEmpty() || authDomains.IsEmpty())
            return false;
         const AnsiString padded = " " + authDomains + " ";
         return padded.Find(" dmarc=" + domain + " ") >= 0;
      }

      // Who is asking: whether the organiser an invitation names is who sent
      // it, from what the server recorded when the message arrived and never
      // from a header (ResourceBooking.h). why says what failed. requireNamed
      // is whether an organiser from outside must also be among the bookers:
      // for a request, yes; for the CANCEL of a booking the room already holds
      // from that organiser, no - taking a partner off the list must not leave
      // their bookings standing where they cannot cancel them.
      bool IsAuthentic(const BookingPolicy &policy, const AnsiString &organiser, const BookingOrigin &origin, AnsiString &why, bool requireNamed = true)
      {
         if (organiser.IsEmpty())
         {
            why = "it names no organiser";
            return false;
         }

         if (IsLocalAddress(organiser))
         {
            if (origin.submitterAccountId <= 0)
            {
               why = "its organiser is an address of this server, and it was not sent by a signed-in session of this server";
               return false;
            }
            std::shared_ptr<const Account> submitter = CacheContainer::Instance()->GetAccount(origin.submitterAccountId);
            if (!submitter)
            {
               why = "the account that sent it no longer exists";
               return false;
            }
            String reason;
            if (!SMTPConnection::SenderPermittedFor(submitter->GetAddress(), WideOf(organiser), reason))
            {
               why = "its organiser is neither the account that sent it nor an address that account may send as";
               return false;
            }
            return true;
         }

         if (requireNamed && !NamedDirectly(policy, organiser))
         {
            why = "its organiser is outside this server's domains and is not named, by address or @domain, among those who may book";
            return false;
         }
         if (!Vouched(origin.authDomains, DomainPart(organiser)))
         {
            why = "its organiser is outside this server's domains, and DMARC did not pass for " + DomainPart(organiser) + " with the organiser as the message's From";
            return false;
         }
         return true;
      }

      // The resource's own bookings over [from, to), as a conflict check reads
      // them - not free-busy's rules. Every stored event that is not cancelled
      // is busy, whatever its TRANSP ("show as free" is a guest's setting, not
      // a room's) and whatever PARTSTAT any attendee has on it, the room's
      // own included: what the calendar holds, the room holds. The UID being
      // decided is left out whole, or only its one occurrence named by
      // exceptRecurrenceId. undecidable, with the reason, when a booking could
      // not be read or expanded in full; false when a calendar could not be
      // read at all.
      bool BookedPeriods(__int64 accountId, __int64 from, __int64 to, const String &uid, bool exceptWhole, bool exceptOne, __int64 exceptRecurrenceId,
                         std::vector<std::pair<__int64, __int64> > &periods, bool &undecidable, AnsiString &problem)
      {
         periods.clear();
         undecidable = false;

         std::vector<CalendarRecord> calendars;
         if (!CalendarStore::ListCalendarsIfAny(accountId, calendars))
         {
            problem = "its calendars could not be read";
            return false;
         }

         for (size_t c = 0; c < calendars.size(); c++)
         {
            std::vector<CalendarObjectRecord> records;
            if (!CalendarStore::ListInRange(accountId, calendars[c].id, from - 86400, to + 86400, records))
            {
               problem = "its calendar could not be read";
               return false;
            }

            for (size_t r = 0; r < records.size(); r++)
            {
               if (records[r].component.CompareNoCase(_T("VEVENT")) != 0)
                  continue;
               const bool same = records[r].uid == uid;
               if (same && exceptWhole)
                  continue;

               ICalComponent tree;
               AnsiString readProblem;
               if (!ICalendar::Parse(Utf8Of(records[r].data), tree, readProblem))
               {
                  undecidable = true;
                  problem = "a booking in its calendar cannot be read";
                  return true;
               }

               std::vector<ICalInstance> instances;
               bool truncated = false;
               if (!ICalendar::Expand(tree, from, to, ICalendar::MaxInstances, instances, truncated, readProblem))
               {
                  undecidable = true;
                  problem = "a booking in its calendar cannot be expanded: " + readProblem;
                  return true;
               }
               if (truncated)
               {
                  undecidable = true;
                  problem = "a booking in its calendar has more occurrences in that time than can be checked";
                  return true;
               }

               for (size_t i = 0; i < instances.size(); i++)
               {
                  const ICalInstance &instance = instances[i];
                  if (instance.component >= tree.children.size())
                     continue;
                  AnsiString status = Trimmed(tree.children[instance.component].Value("STATUS"));
                  status.ToUpper();
                  if (status == "CANCELLED")
                     continue;
                  if (same && exceptOne && instance.recurrenceId == exceptRecurrenceId)
                     continue;
                  const __int64 end = instance.end > instance.start ? instance.end : instance.start + 1;
                  periods.push_back(std::make_pair(instance.start, end));
               }
            }
         }

         return true;
      }

      // One decision about a request.
      struct BookingDecision
      {
         BookingDecision() : failed(false), undecidable(false), accept(false) { }

         bool failed;        // no decision could be reached for a fault; reason says why, for the error log
         bool undecidable;   // no decision could be reached within the limits; reason says why, for the delegates
         bool accept;
         AnsiString reason;  // why it was declined, in a sentence the organiser reads
      };

      // The policy applied to the object: its instances to come, the window,
      // the longest booking, the working hours and the resource's bookings.
      // exceptWhole leaves the UID's booking out of the conflicts (a series
      // decided again); exceptOne leaves out only its occurrence at
      // exceptRecurrenceId (one occurrence decided again).
      BookingDecision Decide(const Account &resource, const BookingPolicy &policy, const ICalComponent &tree, __int64 now,
                             const String &uid, bool exceptWhole, bool exceptOne, __int64 exceptRecurrenceId)
      {
         BookingDecision decision;
         const AnsiString name = DisplayNameOf(resource);

         ICalendar::Summary summary;
         AnsiString problem;
         if (!ICalendar::Summarize(tree, summary, problem))
         {
            decision.failed = true;
            decision.reason = "the invitation's times cannot be read: " + problem;
            return decision;
         }

         if (summary.recurring && !policy.allowRecurring)
         {
            decision.reason = name + " does not take recurring meetings; book each occurrence on its own.";
            return decision;
         }

         // The window counts from this moment, not from the start of today.
         const __int64 horizon = now + static_cast<__int64>(policy.windowDays) * 86400;
         if (summary.last >= ICalendar::Forever)
         {
            decision.reason = name + " can be booked at most " + IntText(policy.windowDays) + " days ahead, and this series has no end.";
            return decision;
         }

         std::vector<ICalInstance> instances;
         bool truncated = false;
         if (!ICalendar::Expand(tree, summary.first, summary.last + 1, ICalendar::MaxInstances, instances, truncated, problem))
         {
            decision.failed = true;
            decision.reason = "the invitation's occurrences cannot be expanded: " + problem;
            return decision;
         }
         if (truncated)
         {
            decision.reason = "This series has more occurrences than " + name + " can check; book it in shorter runs.";
            return decision;
         }

         // The occurrences still to come: one already over is nobody's to
         // decide about, and a series under way is judged from now on.
         std::vector<ICalInstance> future;
         for (size_t i = 0; i < instances.size(); i++)
         {
            const __int64 finish = instances[i].end > instances[i].start ? instances[i].end : instances[i].start + 1;
            if (finish > now)
               future.push_back(instances[i]);
         }
         if (future.empty())
         {
            decision.reason = "This meeting is in the past.";
            return decision;
         }

         const AnsiString zone = Utf8Of(policy.timeZone);
         if (policy.workingHoursOnly && !ICalendar::KnowsTimeZone(zone))
         {
            decision.failed = true;
            decision.reason = "the booking policy's time zone, " + zone + ", is not one this server knows";
            return decision;
         }

         __int64 from = 0;
         __int64 to = 0;
         for (size_t i = 0; i < future.size(); i++)
         {
            const ICalInstance &instance = future[i];
            const AnsiString when = Imip::WhenText(instance.start, instance.end, instance.allDay);

            if (instance.start > horizon)
            {
               decision.reason = name + " can be booked at most " + IntText(policy.windowDays) + " days ahead, and " +
                  (future.size() > 1 ? AnsiString("an occurrence of this series") : AnsiString("this meeting")) + " is on " + when + ".";
               return decision;
            }

            const __int64 minutes = (instance.end - instance.start + 59) / 60;
            if (policy.maxMinutes > 0 && minutes > policy.maxMinutes)
            {
               decision.reason = name + " can be booked for at most " + IntText(policy.maxMinutes) + " minutes at a time, and this meeting lasts " + IntText(minutes) + ".";
               return decision;
            }

            if (policy.workingHoursOnly && !InWorkingHours(policy, zone, instance))
            {
               decision.reason = name + " can be booked in working hours only - " + ClockText(policy.workStart) + " to " + ClockText(policy.workEnd) +
                  " " + zone + ", on its working days - and " + (future.size() > 1 ? AnsiString("an occurrence of this series") : AnsiString("this meeting")) +
                  ", " + when + ", is not.";
               return decision;
            }

            const __int64 start = instance.start;
            const __int64 finish = instance.end > instance.start ? instance.end : instance.start + 1;
            if (from == 0 || start < from)
               from = start;
            if (finish > to)
               to = finish;
         }

         if (!policy.allowConflicts && to > from)
         {
            // Read a day either side of what is asked for, so that a booking
            // that began before the asked-for time is answered whole rather
            // than clipped to it; only an overlap with an occurrence counts.
            std::vector<std::pair<__int64, __int64> > periods;
            bool undecidable = false;
            if (!BookedPeriods(resource.GetID(), from - 86400, to + 86400, uid, exceptWhole, exceptOne, exceptRecurrenceId, periods, undecidable, problem))
            {
               decision.failed = true;
               decision.reason = problem;
               return decision;
            }
            if (undecidable)
            {
               decision.undecidable = true;
               decision.reason = problem;
               return decision;
            }

            // Joined into disjoint periods, sorted, so each occurrence is one
            // search; what the answer says of a conflict is when, never whose.
            std::sort(periods.begin(), periods.end());
            std::vector<std::pair<__int64, __int64> > joined;
            for (size_t p = 0; p < periods.size(); p++)
            {
               if (!joined.empty() && periods[p].first <= joined.back().second)
                  joined.back().second = std::max(joined.back().second, periods[p].second);
               else
                  joined.push_back(periods[p]);
            }

            std::set<std::pair<__int64, __int64> > conflicts;
            for (size_t i = 0; i < future.size(); i++)
            {
               const __int64 start = future[i].start;
               const __int64 finish = future[i].end > future[i].start ? future[i].end : future[i].start + 1;
               std::vector<std::pair<__int64, __int64> >::const_iterator found =
                  std::lower_bound(joined.begin(), joined.end(), start,
                                   [](const std::pair<__int64, __int64> &period, __int64 value) { return period.second <= value; });
               for (; found != joined.end() && found->first < finish; ++found)
                  conflicts.insert(*found);
            }

            if (!conflicts.empty())
            {
               AnsiString list;
               size_t named = 0;
               for (std::set<std::pair<__int64, __int64> >::const_iterator it = conflicts.begin(); it != conflicts.end() && named < ConflictsNamed; ++it, ++named)
                  list += (named > 0 ? "; " : "") + Imip::WhenText(it->first, it->second, false);
               if (conflicts.size() > named)
                  list += " and " + IntText(static_cast<__int64>(conflicts.size() - named)) + " more";
               decision.reason = name + " is already booked at that time: " + list + ".";
               return decision;
            }
         }

         decision.accept = true;
         return decision;
      }

      // A COMMENT on the answer's event, which a client shows beside it.
      AnsiString WithComment(const AnsiString &calendarText, const AnsiString &comment)
      {
         ICalComponent tree;
         AnsiString problem;
         if (comment.IsEmpty() || !ICalendar::Parse(calendarText, tree, problem))
            return calendarText;
         for (size_t i = 0; i < tree.children.size(); i++)
         {
            if (!IsEventLike(tree.children[i]))
               continue;
            ICalProperty property;
            property.name = "COMMENT";
            property.value = ICalendar::Escape(comment);
            tree.children[i].properties.push_back(property);
         }
         return ICalendar::Serialize(tree);
      }

      // A REPLY that quotes nothing of the request's own text, for a request
      // that is not honoured: the UID, the SEQUENCE, the RECURRENCE-ID, the
      // stamp, the organiser's bare address and the resource's own answer.
      // No SUMMARY, times, LOCATION or organiser's CN, and a time zone only
      // when the RECURRENCE-ID needs one to be read.
      AnsiString WithoutRequestText(const AnsiString &calendarText)
      {
         ICalComponent tree;
         AnsiString problem;
         if (!ICalendar::Parse(calendarText, tree, problem))
            return calendarText;

         bool zoneNeeded = false;
         for (size_t c = 0; c < tree.children.size(); c++)
         {
            ICalComponent &component = tree.children[c];
            if (!IsEventLike(component))
               continue;
            std::vector<ICalProperty> kept;
            for (size_t p = 0; p < component.properties.size(); p++)
            {
               ICalProperty property = component.properties[p];
               if (property.name == "ORGANIZER")
                  property.parameters.clear();
               else if (property.name == "RECURRENCE-ID")
                  zoneNeeded = zoneNeeded || !property.Parameter("TZID").IsEmpty();
               else if (property.name != "UID" && property.name != "SEQUENCE" && property.name != "DTSTAMP" && property.name != "ATTENDEE")
                  continue;
               kept.push_back(property);
            }
            component.properties = kept;
         }

         if (!zoneNeeded)
         {
            for (size_t c = 0; c < tree.children.size();)
            {
               if (tree.children[c].name == "VTIMEZONE")
                  tree.children.erase(tree.children.begin() + c);
               else
                  c++;
            }
         }

         return ICalendar::Serialize(tree);
      }

      // 0 when queued; 1 when the recipient was refused (not an address, not
      // a destination this server delivers to, restricted delivery); 2 when
      // the server failed to write or queue it.
      // delegate, when given, is the account that answered for the resource:
      // the envelope and the restrictions are theirs and the message is their
      // submission, as the REST reply's are (RestApiServer::QueueRawMessage_),
      // so a bounce goes back to them. Otherwise the resource is sending.
      int QueueMail(std::shared_ptr<const Account> resource, const AnsiString &toUtf8, const AnsiString &rawText, AnsiString &problem,
                    std::shared_ptr<const Account> delegate = std::shared_ptr<const Account>())
      {
         const String to = WideOf(toUtf8);
         if (!StringParser::IsValidEmailAddress(to))
         {
            problem = toUtf8 + " is not an e-mail address";
            return 1;
         }

         std::shared_ptr<const Account> sender = delegate ? delegate : resource;
         std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
         message->SetFromAddress(sender->GetAddress());

         RecipientParser parser;
         String reason;
         bool treatSecurityAsLocal = false;
         RecipientParser::DeliveryPossibility possibility =
            parser.CheckDeliveryPossibility(true, sender->GetAddress(), to, reason, treatSecurityAsLocal, 0, true);
         if (possibility != RecipientParser::DP_Possible)
         {
            problem = toUtf8 + ": " + (reason.IsEmpty() ? AnsiString("delivery is not permitted") : Utf8Of(reason));
            return 1;
         }

         // A resource is an account like any other here: a restricted one
         // answers named domains only, in the sentence RCPT TO uses; and so
         // does a restricted delegate answering for it.
         if (RestrictedDelivery::Check(sender, to, RestrictedDelivery::Outbound) == RestrictedDelivery::Refused)
         {
            problem = Utf8Of(RestrictedDelivery::OutboundRefusal(to));
            return 1;
         }

         bool recipientOK = false;
         parser.CreateMessageRecipientList(to, message->GetRecipients(), recipientOK);
         if (!recipientOK || message->GetRecipients()->GetCount() == 0)
         {
            problem = toUtf8 + ": unknown recipient";
            return 1;
         }

         const String fileName = MessageStore::WorkingFile(message);
         if (!FileUtilities::WriteToFile(fileName, rawText))
         {
            problem = "the message could not be written";
            return 2;
         }

         message->SetSize((int) MessageStore::FileSize(fileName));
         // The server writing for an account of its own, as the webmail's
         // answers are (Message::FlagSubmittedBySignedInUser). The submitter is
         // the delegate who answered, when one did; otherwise the resource
         // itself, which only this server ever sends as (nobody can sign in as
         // it), as a scheduled send records the account it sends for. An
         // organiser's calendar on this server files the answer only when the
         // server recorded who sent it (Imip::IsSentBy).
         message->SetSubmittedBySignedInUser(true);
         message->SetSubmitterAccountID(delegate ? delegate->GetID() : resource->GetID());
         message->SetState(Message::Delivering);
         if (!PersistentMessage::SaveObject(message))
         {
            FileUtilities::DeleteFile(fileName);
            problem = "the message could not be queued";
            return 2;
         }

         Application::Instance()->SubmitPendingEmail();
         return 0;
      }

      // The plain message the delegates are sent about a request.
      AnsiString NoticeMail(const Account &resource, const AnsiString &to, const AnsiString &subject, const AnsiString &body)
      {
         const AnsiString address = Utf8Of(resource.GetAddress());
         const AnsiString domain = DomainPart(address).IsEmpty() ? address : DomainPart(address);
         const char *crlf = "\r\n";

         AnsiString text;
         text += "From: " + HeaderSafe(FromHeaderOf(resource)) + crlf;
         text += "To: <" + HeaderSafe(to) + ">" + crlf;
         text += "Subject: " + EncodedWord(HeaderSafe(subject)) + crlf;
         text += "Date: " + Utf8Of(Time::GetCurrentMimeDate()) + crlf;
         text += "Message-ID: <" + Utf8Of(GUIDCreator::GetGUID()) + "@" + domain + ">" + crlf;
         text += "MIME-Version: 1.0" + AnsiString(crlf);
         text += "Auto-Submitted: auto-generated" + AnsiString(crlf);
         text += "Content-Type: text/plain; charset=utf-8" + AnsiString(crlf);
         text += "Content-Transfer-Encoding: base64" + AnsiString(crlf) + crlf;
         text += Base64Lines(body);
         return text;
      }

      // ------------------------------------------------------------------
      // The answer memory, hm_bookinganswers: per resource and organiser,
      // the answers of the current hour and when the delegates were last
      // told of a request that was not honoured. Read and written under the
      // resource's lock, so one process never races itself on a row.
      // ------------------------------------------------------------------

      struct AnswerMemory
      {
         AnswerMemory() : found(false), id(0), window(0), answers(0), noticed(0) { }

         bool found;
         __int64 id;
         __int64 window;
         int answers;
         __int64 noticed;
      };

      // The key a memory row is kept under: the organiser as resolved, else
      // the envelope sender, else "<>"; lower-cased, at most 255 characters.
      String MemoryKeyOf(const AnsiString &organiser, const String &envelopeSender)
      {
         String key = !organiser.IsEmpty() ? WideOf(organiser) : WideOf(Normalised(envelopeSender));
         if (key.IsEmpty())
            key = _T("<>");
         key.ToLower();
         if (key.GetLength() > 255)
            key = key.Mid(0, 255);
         return key;
      }

      bool ReadMemory(__int64 accountId, const String &key, AnswerMemory &memory)
      {
         memory = AnswerMemory();
         SQLCommand command("select baid, bawindow, baanswers, banoticed from hm_bookinganswers where baaccountid = @ACCOUNTID and baorganiser = @ORGANISER");
         command.AddParameter("@ACCOUNTID", accountId);
         command.AddParameter("@ORGANISER", key);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;
         if (recordset->IsEOF())
            return true;
         memory.found = true;
         memory.id = recordset->GetInt64Value("baid");
         memory.window = recordset->GetInt64Value("bawindow");
         memory.answers = static_cast<int>(recordset->GetLongValue("baanswers"));
         memory.noticed = recordset->GetInt64Value("banoticed");
         return true;
      }

      bool WriteMemory(__int64 accountId, const String &key, const AnswerMemory &memory, __int64 now)
      {
         if (memory.found)
         {
            SQLCommand update("update hm_bookinganswers set bawindow = @WINDOW, baanswers = @ANSWERS, banoticed = @NOTICED where baid = @ID");
            update.AddParameter("@WINDOW", memory.window);
            update.AddParameter("@ANSWERS", memory.answers);
            update.AddParameter("@NOTICED", memory.noticed);
            update.AddParameter("@ID", memory.id);
            return Application::Instance()->GetDBManager()->Execute(update);
         }

         // A new row, and this resource's rows a day stale go: the memory is
         // an hour long, and a row per organiser ever seen would only grow.
         SQLCommand prune("delete from hm_bookinganswers where baaccountid = @ACCOUNTID and bawindow < @STALE1 and banoticed < @STALE2");
         prune.AddParameter("@ACCOUNTID", accountId);
         prune.AddParameter("@STALE1", now - 86400);
         prune.AddParameter("@STALE2", now - 86400);
         Application::Instance()->GetDBManager()->Execute(prune);

         SQLStatement statement;
         statement.SetTable("hm_bookinganswers");
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("baid");
         statement.AddColumnInt64("baaccountid", accountId);
         statement.AddColumn("baorganiser", key);
         statement.AddColumnInt64("bawindow", memory.window);
         statement.AddColumnInt64("baanswers", memory.answers);
         statement.AddColumnInt64("banoticed", memory.noticed);
         return Application::Instance()->GetDBManager()->Execute(statement);
      }

      // Whether an answer may go to the organiser now, counted when it may.
      // A memory that cannot be read sends nothing.
      bool MayAnswer(__int64 accountId, const String &key, __int64 now)
      {
         AnswerMemory memory;
         if (!ReadMemory(accountId, key, memory))
            return false;
         if (memory.window + MemoryHour <= now)
         {
            memory.window = now;
            memory.answers = 0;
         }
         if (memory.answers >= ResourceBooking::AnswersPerHour)
            return false;
         memory.answers++;
         WriteMemory(accountId, key, memory, now);
         return true;
      }

      // The resource-wide ceilings on notices, per hour, kept in memory: a
      // sender who writes a new organiser into every message is one notice
      // an hour per organiser, and NoticesPerHour is what bounds the
      // organisers of requests not honoured; AllNoticesPerHour bounds every
      // notice the resource sends, so that believed organisers rotated to get
      // round their own budgets are bounded too. Either can hold a genuine
      // notice back for the rest of an hour; the request is in the mailbox
      // all the same.
      std::mutex noticesGuard;
      std::map<__int64, std::pair<__int64, int> > noticeWindows;
      std::map<__int64, std::pair<__int64, int> > allNoticeWindows;

      std::pair<__int64, int> &CurrentWindow(std::map<__int64, std::pair<__int64, int> > &windows, __int64 accountId, __int64 now)
      {
         std::pair<__int64, int> &window = windows[accountId];
         if (window.first + MemoryHour <= now)
         {
            window.first = now;
            window.second = 0;
         }
         return window;
      }

      // A notice counted against AllNoticesPerHour and, for one about a
      // request not honoured or past its organiser's hour, against
      // NoticesPerHour as well: both ceilings are checked before either is
      // counted, so a notice one of them refuses spends nothing of the other.
      bool WithinRoomNotices(__int64 accountId, __int64 now, bool alsoUnanswered = false)
      {
         std::lock_guard<std::mutex> guard(noticesGuard);
         std::pair<__int64, int> &all = CurrentWindow(allNoticeWindows, accountId, now);
         if (all.second >= ResourceBooking::AllNoticesPerHour)
            return false;
         if (alsoUnanswered)
         {
            std::pair<__int64, int> &some = CurrentWindow(noticeWindows, accountId, now);
            if (some.second >= ResourceBooking::NoticesPerHour)
               return false;
            some.second++;
         }
         all.second++;
         return true;
      }

      // Whether AllNoticesPerHour has room for one more, counting nothing.
      bool RoomNoticesLeft(__int64 accountId, __int64 now)
      {
         std::lock_guard<std::mutex> guard(noticesGuard);
         return CurrentWindow(allNoticeWindows, accountId, now).second < ResourceBooking::AllNoticesPerHour;
      }

      // The memory key a request nobody verified is counted under: never the
      // named organiser's own, so that forged requests cannot use up the hour
      // of the organiser they name. It starts with a space, which no key of
      // an organiser can (MemoryKeyOf's keys are trimmed addresses, or "<>");
      // a question mark would not do, since "?" may begin an address.
      String UnverifiedKeyOf(const String &key)
      {
         String unverified = _T(" ?") + key;
         if (unverified.GetLength() > 255)
            unverified = unverified.Mid(0, 255);
         return unverified;
      }

      // Whether the delegates may be told now of a request from this organiser
      // that was not honoured: once an hour per organiser, and NoticesPerHour
      // an hour per resource. Stamped when they may.
      bool MayNotice(__int64 accountId, const String &key, __int64 now)
      {
         AnswerMemory memory;
         if (!ReadMemory(accountId, key, memory))
            return false;
         if (memory.found && memory.noticed + MemoryHour > now)
            return false;
         if (!WithinRoomNotices(accountId, now, true))
            return false;

         memory.noticed = now;
         WriteMemory(accountId, key, memory, now);
         return true;
      }

      // ------------------------------------------------------------------
      // A decision's result, and the work done with it once the lock is
      // released.
      // ------------------------------------------------------------------

      struct BookingOutcome
      {
         BookingOutcome() : reply(false), accept(false), leave(false), unhonoured(false), tell(false), errorCode(0) { }

         bool reply;             // answer the organiser
         bool accept;
         AnsiString reason;      // the sentence in a decline
         bool leave;             // left for the delegates, the organiser being who wrote it
         bool unhonoured;        // not the organiser's, or not one the policy lets book
         bool tell;              // the delegates are told
         AnsiString why;         // why it was left, or not honoured
         AnsiString where;       // where the delegates find a request left for them
         int errorCode;          // 7430-7434 when a fault is the reason, else 0
         AnsiString error;       // the error log's detail
      };

      // ------------------------------------------------------------------
      // What the resource decided (hm_bookingdecisions): the organiser it
      // believed when it accepted the booking under a UID. An answer a
      // delegate gives later - a decline over REST or CalDAV - goes to that
      // organiser and to nobody else, whatever the calendar object says by
      // then: anybody with a write share of the resource's calendar can write
      // any ORGANIZER into it. Written and removed under the resource's lock.
      //
      // The UID is compared exactly, as the calendar compares it
      // (CalendarStore's ReadExactObject). bduid is compared by the database
      // under the column's collation - one that ignores case and width on SQL
      // Server and Compact, and trailing spaces under MySQL's utf8mb4_bin - so
      // the WHERE clause only narrows the rows, and each is checked here and
      // removed by its id. Keyed on the collation alone, a booking whose UID
      // differed from another's only in letters the collation does not tell
      // apart removed that booking's record and wrote its own organiser in its
      // place, and a delegate's later decline of the first booking told nobody.
      // ------------------------------------------------------------------

      struct DecisionRow
      {
         DecisionRow() : id(0) { }

         __int64 id;
         String uid;
         String organiser;
      };

      // The rows the database finds under the UID, exact or not. False when
      // the table could not be read.
      bool ReadDecisionRows(__int64 accountId, const String &uid, std::vector<DecisionRow> &rows)
      {
         rows.clear();
         SQLCommand command("select bdid, bduid, bdorganiser from hm_bookingdecisions where bdaccountid = @ACCOUNTID and bduid = @UID");
         command.AddParameter("@ACCOUNTID", accountId);
         command.AddParameter("@UID", uid);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;
         while (!recordset->IsEOF())
         {
            DecisionRow row;
            row.id = recordset->GetInt64Value("bdid");
            row.uid = recordset->GetStringValue("bduid");
            row.organiser = recordset->GetStringValue("bdorganiser");
            rows.push_back(row);
            recordset->MoveNext();
         }
         return true;
      }

      bool DeleteDecisionRow(__int64 id)
      {
         SQLCommand command("delete from hm_bookingdecisions where bdid = @ID");
         command.AddParameter("@ID", id);
         return Application::Instance()->GetDBManager()->Execute(command);
      }

      bool RecordDecision(__int64 accountId, const String &uid, const AnsiString &organiser)
      {
         const String organiserText = WideOf(organiser);
         if (uid.IsEmpty() || uid.GetLength() > 255 || organiserText.IsEmpty() || organiserText.GetLength() > 255)
            return false;

         std::vector<DecisionRow> rows;
         if (!ReadDecisionRows(accountId, uid, rows))
            return false;

         // Another booking's record, under a UID the collation takes for this
         // one: it is that booking's, and stays. The unique index would refuse a
         // second row beside it on this backend, so this booking goes
         // unrecorded, which the caller logs.
         for (size_t i = 0; i < rows.size(); i++)
         {
            if (rows[i].uid.Compare(uid) != 0)
               return false;
         }

         for (size_t i = 0; i < rows.size(); i++)
         {
            if (!DeleteDecisionRow(rows[i].id))
               return false;
         }

         SQLStatement statement;
         statement.SetTable("hm_bookingdecisions");
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("bdid");
         statement.AddColumnInt64("bdaccountid", accountId);
         statement.AddColumn("bduid", uid);
         statement.AddColumn("bdorganiser", organiserText);
         statement.AddColumnInt64("bddecided", static_cast<__int64>(time(nullptr)));
         return Application::Instance()->GetDBManager()->Execute(statement);
      }

      // False when the table could not be read; organiser "" when the
      // resource accepted no booking under the UID.
      bool ReadDecision(__int64 accountId, const String &uid, AnsiString &organiser)
      {
         organiser = "";
         if (uid.IsEmpty() || uid.GetLength() > 255)
            return true;

         std::vector<DecisionRow> rows;
         if (!ReadDecisionRows(accountId, uid, rows))
            return false;
         for (size_t i = 0; i < rows.size(); i++)
         {
            if (rows[i].uid.Compare(uid) == 0)
            {
               organiser = Utf8Of(LowerWide(rows[i].organiser));
               break;
            }
         }
         return true;
      }

      // Every UID a live object of the account's calendars holds, read once
      // through the calendars (hm_calendarobjects is indexed by calendar, not
      // by account or UID). False when it could not be read.
      bool HeldUidsOf(__int64 accountId, std::set<String> &uids)
      {
         uids.clear();
         SQLCommand command("select objectuid from hm_calendarobjects inner join hm_calendars on objectcalendarid = calendarid where calendaraccountid = @ACCOUNTID and objectdeleted = 0");
         command.AddParameter("@ACCOUNTID", accountId);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;
         while (!recordset->IsEOF())
         {
            uids.insert(recordset->GetStringValue("objectuid"));
            recordset->MoveNext();
         }
         return true;
      }

      void DropDecision(__int64 accountId, const String &uid)
      {
         if (uid.IsEmpty() || uid.GetLength() > 255)
            return;

         // Only the row recorded under exactly this UID: another booking's under
         // a UID the collation takes for this one is that booking's.
         std::vector<DecisionRow> rows;
         bool removed = ReadDecisionRows(accountId, uid, rows);
         for (size_t i = 0; removed && i < rows.size(); i++)
         {
            if (rows[i].uid.Compare(uid) == 0)
               removed = DeleteDecisionRow(rows[i].id);
         }
         if (!removed)
            LOG_DEBUG("Resource booking: the record of the booking " + uid + " of account " + String(IntText(accountId)) + " could not be removed.");
      }

      // A declined booking taken out of the calendar that held it: the whole
      // booking for a changed series, the one occurrence - an EXDATE, and any
      // change of it the series held - for a changed occurrence. A series is
      // never deleted for one occurrence. True when there was nothing to take
      // out.
      bool TakeOut(__int64 resourceId, const HeldBooking &held, const ICalComponent *occurrence, __int64 recurrenceId,
                   const ICalComponent &requestTree, AnsiString &problem)
      {
         problem = "the calendar could not be written";
         if (!held.found)
            return true;

         __int64 token = 0;
         if (!occurrence)
         {
            if (!CalendarStore::Delete(resourceId, held.calendar.id, held.record.id, token))
               return false;
            DropDecision(resourceId, held.record.uid);
            return true;
         }

         ICalComponent tree = held.tree;
         RemoveOverrides(tree, recurrenceId);
         AdoptTimeZones(tree, requestTree);
         ICalComponent *master = MutableMasterOf(tree);
         const ICalProperty *recurrenceIdProperty = occurrence->Property("RECURRENCE-ID");
         if (master && recurrenceIdProperty && (master->Property("RRULE") || master->Property("RDATE")))
         {
            ICalProperty exdate = *recurrenceIdProperty;
            exdate.name = "EXDATE";
            for (size_t p = 0; p < exdate.parameters.size();)
            {
               if (exdate.parameters[p].first == "RANGE")
                  exdate.parameters.erase(exdate.parameters.begin() + p);
               else
                  p++;
            }
            master->properties.push_back(exdate);
         }

         if (EventCount(tree) == 0)
         {
            if (!CalendarStore::Delete(resourceId, held.calendar.id, held.record.id, token))
               return false;
            DropDecision(resourceId, held.record.uid);
            return true;
         }
         return WriteTree(resourceId, held, tree, problem);
      }

      struct BookingJob
      {
         std::shared_ptr<const Account> resource;
         ImipMessage message;
         BookingOrigin origin;
      };

      void TellDelegates(std::shared_ptr<const Account> resource, const AnsiString &subject, const AnsiString &body)
      {
         std::vector<String> delegates;
         if (!ResourceBooking::DelegatesOf(resource->GetID(), delegates))
         {
            LOG_DEBUG("Resource booking: the delegates of " + resource->GetAddress() + " could not be read.");
            return;
         }

         for (size_t i = 0; i < delegates.size(); i++)
         {
            const AnsiString to = Utf8Of(delegates[i]);
            AnsiString problem;
            if (QueueMail(resource, to, NoticeMail(*resource, to, subject, body), problem) != 0)
               LOG_DEBUG("Resource booking: the delegate " + delegates[i] + " of " + resource->GetAddress() + " was not told: " + String(problem));
         }
      }

      void HandleRequest(const BookingJob &job)
      {
         std::shared_ptr<const Account> resource = job.resource;
         const ImipMessage &request = job.message;
         const BookingOrigin &origin = job.origin;

         const String uid = WideOf(request.uid);
         const AnsiString resourceAddress = Utf8Of(LowerWide(resource->GetAddress()));
         const AnsiString name = DisplayNameOf(*resource);

         // A task handed to a room is nobody's booking.
         if (request.component != "VEVENT" || request.uid.IsEmpty())
         {
            LOG_DEBUG("Resource booking: a REQUEST to " + resource->GetAddress() + " that is not an event with a UID was not decided.");
            return;
         }

         ImipAttendee attendee;
         if (!Imip::AttendeeFor(request, resourceAddress, attendee))
         {
            LOG_DEBUG("Resource booking: a REQUEST for " + uid + " does not name " + resource->GetAddress() + " as a guest and was not decided.");
            return;
         }

         const AnsiString organiser = Normalised(WideOf(request.organizer));
         if (!organiser.IsEmpty() && organiser == resourceAddress)
            return;

         // One changed occurrence of a series, without the series.
         const bool occurrenceOnly = !HasMaster(request.tree);
         __int64 recurrenceId = 0;
         const ICalComponent *occurrence = nullptr;
         if (occurrenceOnly && EventCount(request.tree) == 1)
         {
            occurrence = MasterOf(request.tree);
            if (occurrence && !RecurrenceIdOf(*occurrence, request.tree, recurrenceId))
               occurrence = nullptr;
         }

         const String memoryKey = MemoryKeyOf(organiser, origin.envelopeSender);

         BookingOutcome outcome;
         {
            std::shared_ptr<std::recursive_mutex> lock = LockOf(resource->GetID());
            std::lock_guard<std::recursive_mutex> guard(*lock);
            const __int64 now = static_cast<__int64>(time(nullptr));

            BookingPolicy policy;
            HeldBooking held;
            bool calendarRead = false;
            bool allowed = false;
            bool budgeted = false;     // this request has had its place in the organiser's hour
            AnsiString notAuthentic;

            // RFC 3834: nothing automatic answers what was sent automatically,
            // or what has no return path to answer.
            const bool automatic = origin.envelopeSender.IsEmpty() || origin.autoSubmitted;

            // Whether the organiser is who sent it: nobody is, without the
            // policy, since an organiser from outside needs its list.
            const bool policyRead = ResourceBooking::GetPolicy(resource->GetID(), policy);
            const bool believed = policyRead && IsAuthentic(policy, organiser, origin, notAuthentic);

            if (!policyRead)
            {
               // Nobody is verified without the policy (an organiser from
               // outside needs its list), so the request is treated as one not
               // honoured: no organiser's hour is spent, and the notice repeats
               // nothing of it.
               outcome.unhonoured = true;
               outcome.why = "its booking policy could not be read";
               outcome.errorCode = 7430;
            }
            else if (!believed)
            {
               outcome.unhonoured = true;
               outcome.why = notAuthentic;
            }
            else if (!FindHeld(resource->GetID(), uid, held))
            {
               outcome.leave = true;
               outcome.why = "its calendar could not be read";
               outcome.errorCode = 7431;
            }
            else if (held.found && held.organiser != organiser)
            {
               // A UID is mailed to every guest: a REQUEST under the UID of a
               // booking somebody else organises is not a request for it.
               calendarRead = true;
               outcome.unhonoured = true;
               outcome.why = "its calendar holds a booking under the invitation's UID that another organiser made";
            }
            else if (held.found && !occurrenceOnly && HasMaster(held.tree) && held.sequence > request.sequence)
            {
               LOG_APPLICATION("Resource booking: a REQUEST for " + uid + " to " + resource->GetAddress() +
                  " is older than the booking the calendar holds, and was not decided.");
               return;
            }
            else
            {
               calendarRead = true;
               allowed = MayBook(policy, organiser);

               // A changed occurrence is newer than the series the calendar
               // holds and than the override the series has for it, or it is
               // not decided: a late change from before a series-wide update
               // does not undo the update.
               bool older = occurrence && held.found && HasMaster(held.tree) && held.sequence > SequenceOf(*occurrence);
               if (occurrence && held.found && !older)
               {
                  for (size_t i = 0; i < held.tree.children.size() && !older; i++)
                  {
                     __int64 instant = 0;
                     if (IsEventLike(held.tree.children[i]) && RecurrenceIdOf(held.tree.children[i], held.tree, instant) && instant == recurrenceId)
                        older = SequenceOf(held.tree.children[i]) > SequenceOf(*occurrence);
                  }
               }
               if (older)
               {
                  LOG_APPLICATION("Resource booking: a REQUEST for one occurrence of " + uid + " to " + resource->GetAddress() +
                     " is older than the change of it the calendar holds, and was not decided.");
                  return;
               }

               if (!allowed)
               {
                  // Never filed, and the delegates told once an hour. The
                  // organiser is who wrote it, so it is declined - when an
                  // answer may be sent at all - and a booking the calendar
                  // held from before the policy changed is taken out with it.
                  outcome.unhonoured = true;
                  outcome.why = "its organiser is not one of those allowed to book it";
                  if (!automatic && policy.autoAccept && (!occurrenceOnly || occurrence) && MayAnswer(resource->GetID(), memoryKey, now))
                  {
                     AnsiString problem;
                     if (TakeOut(resource->GetID(), held, occurrence, recurrenceId, request.tree, problem))
                     {
                        outcome.reply = true;
                        outcome.accept = false;
                        outcome.reason = "You are not one of the people allowed to book " + name + ".";
                     }
                     else
                     {
                        outcome.errorCode = 7432;
                        outcome.error = problem;
                        outcome.why = "the booking it declined could not be taken out of its calendar";
                     }
                  }
               }
               else if (automatic)
               {
                  outcome.leave = true;
                  outcome.why = "it was sent automatically, and an automatic answer is never sent to an automatic message (RFC 3834)";
               }
               else if (!policy.autoAccept)
               {
                  outcome.leave = true;
                  outcome.why = "it answers no invitation automatically";
               }
               else if (occurrenceOnly && !occurrence)
               {
                  outcome.leave = true;
                  outcome.why = "it changes several occurrences of a series in one message, which is left for a person";
               }
               else if (!MayAnswer(resource->GetID(), memoryKey, now))
               {
                  outcome.leave = true;
                  outcome.why = "its organiser has had " + IntText(ResourceBooking::AnswersPerHour) + " answers from it in the past hour";
               }
               else
               {
                  budgeted = true;
                  const BookingDecision decision = Decide(*resource, policy, request.tree, now, uid, occurrence == nullptr, occurrence != nullptr, recurrenceId);
                  if (decision.failed)
                  {
                     outcome.leave = true;
                     outcome.why = "a decision could not be reached";
                     outcome.errorCode = 7431;
                     outcome.error = decision.reason;
                  }
                  else if (decision.undecidable)
                  {
                     outcome.leave = true;
                     outcome.why = "a decision could not be reached: " + decision.reason;
                  }
                  else if (decision.accept)
                  {
                     AnsiString problem;
                     bool written = false;
                     if (occurrence && held.found)
                     {
                        // Merged into the series: its override of this
                        // occurrence replaced, and nothing else of it touched.
                        ICalComponent tree = held.tree;
                        RemoveOverrides(tree, recurrenceId);
                        AdoptTimeZones(tree, request.tree);
                        ICalComponent single;
                        single.name = "VCALENDAR";
                        single.children.push_back(*occurrence);
                        Imip::SetPartstat(single, attendee.address, name, "ACCEPTED");
                        tree.children.push_back(single.children[0]);
                        written = WriteTree(resource->GetID(), held, tree, problem);
                     }
                     else
                     {
                        // The organiser was believed above: the room's own
                        // booking of the request, never marked as filed
                        // unverified (Imip::KeepRule).
                        CalendarRecord calendar;
                        CalendarObjectRecord record;
                        written = Imip::KeepInvitation(resource->GetID(), request, true, false, attendee.address, name, "ACCEPTED", calendar, record, problem,
                                                       ImipProvenance::FromOrganiser);
                     }

                     if (written)
                     {
                        outcome.reply = true;
                        outcome.accept = true;

                        // Whom it was accepted from, as the resource believed
                        // it: where a delegate's later answer about it goes.
                        if (!RecordDecision(resource->GetID(), uid, organiser))
                        {
                           LOG_APPLICATION("Resource booking: the booking " + uid + " of " + resource->GetAddress() +
                              " was accepted, and whom it was accepted from could not be recorded: a delegate's later answer about it will tell nobody.");
                        }
                     }
                     else
                     {
                        outcome.leave = true;
                        outcome.why = "the booking could not be written to its calendar";
                        outcome.errorCode = 7432;
                        outcome.error = problem;
                     }
                  }
                  else
                  {
                     // A booking the calendar held is taken out of it, so the
                     // slot is free again and the calendar shows what the
                     // resource agreed to and nothing it refused.
                     AnsiString problem;
                     if (TakeOut(resource->GetID(), held, occurrence, recurrenceId, request.tree, problem))
                     {
                        outcome.reply = true;
                        outcome.accept = false;
                        outcome.reason = decision.reason;
                     }
                     else
                     {
                        outcome.leave = true;
                        outcome.why = "the booking it declined could not be taken out of its calendar";
                        outcome.errorCode = 7432;
                        outcome.error = problem;
                     }
                  }
               }
            }

            // A request left for the delegates is never put into the room's
            // calendar: the room has not decided it, and a booking filed as the
            // sender wrote it would hold the slot on the sender's word. It is in
            // the room's mailbox, and the delegates are told what it asks.
            if (calendarRead && held.found)
               outcome.where = "The calendar of " + name + " holds this booking as it was before this request, and it stays so until its organiser or a delegate changes it; the request is in the mailbox of " + name + ".";
            else
               outcome.where = "It is in the mailbox of " + name + ", and not in its calendar: nothing the room has not decided is booked.";

            // The delegates are told of a request left for them within its
            // organiser's hour of answers - the same budget an answer takes - and
            // of one past it, or one not honoured, once an hour for each
            // organiser and NoticesPerHour an hour for the room.
            if (outcome.leave && (budgeted || (RoomNoticesLeft(resource->GetID(), now) && MayAnswer(resource->GetID(), memoryKey, now))))
               outcome.tell = WithinRoomNotices(resource->GetID(), now);
            else if (outcome.leave || outcome.unhonoured)
               outcome.tell = MayNotice(resource->GetID(), believed ? memoryKey : UnverifiedKeyOf(memoryKey), now);
         }

         if (outcome.errorCode != 0)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, outcome.errorCode, "ResourceBooking::HandleRequest",
               "The invitation " + uid + " to " + resource->GetAddress() + " was left in its mailbox for its delegates because " + String(outcome.why) +
               (outcome.error.IsEmpty() ? String(_T(".")) : String(_T(": ")) + String(outcome.error) + _T(".")));
         }

         const AnsiString when = Imip::WhenText(request.start, request.end, request.allDay);

         if (outcome.reply)
         {
            const AnsiString partstat = outcome.accept ? "ACCEPTED" : "DECLINED";
            const AnsiString cutype = resource->GetKind() == Account::KindRoom ? "ROOM" : "RESOURCE";

            // The resource's own name on its answer, never the name the
            // request gave it.
            AnsiString replyText = Imip::ReplyObject(request, attendee.address, name, partstat, AnsiString(), cutype);
            if (!outcome.accept)
               replyText = WithoutRequestText(replyText);
            replyText = WithComment(replyText, outcome.reason);

            // An answer to a request that is honoured may repeat it; one that
            // is not quotes nothing of what the request said.
            AnsiString subject;
            AnsiString body;
            if (outcome.accept)
            {
               subject = "Accepted: " + ICalendar::Unescape(request.summary) + " @ " + when;
               body = name + " <" + resourceAddress + "> has accepted this invitation.\r\n\r\n" + Imip::Describe(request);
            }
            else
            {
               subject = "Declined: " + name + ", " + when;
               body = name + " <" + resourceAddress + "> has declined this invitation: " + outcome.reason + "\r\n";
            }

            // Auto-Submitted, so the organiser's own automatic reply does not
            // answer the answer (RFC 3834).
            const AnsiString mail = "Auto-Submitted: auto-replied\r\n" +
               Imip::BuildMail(FromHeaderOf(*resource), organiser, subject, body, "REPLY", replyText, resourceAddress);

            AnsiString problem;
            const int status = QueueMail(resource, organiser, mail, problem);
            if (status == 0)
            {
               LOG_APPLICATION("Resource booking: " + resource->GetAddress() + " " + String(outcome.accept ? _T("accepted") : _T("declined")) + " the invitation " + uid +
                  " from " + String(organiser) + (outcome.accept ? String() : String(_T(": ")) + String(outcome.reason)));
            }
            else if (status == 2)
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7433, "ResourceBooking::HandleRequest",
                  "The answer of " + resource->GetAddress() + " to the invitation " + uid + " was decided and filed, and could not be queued to " +
                  String(organiser) + ": " + String(problem) + ".");
            }
            else
            {
               LOG_APPLICATION("Resource booking: the answer of " + resource->GetAddress() + " to the invitation " + uid +
                  " was not sent: " + String(problem));
            }
         }

         if (outcome.leave && outcome.errorCode == 0)
         {
            LOG_APPLICATION("Resource booking: the invitation " + uid + " to " + resource->GetAddress() + " was left for its delegates: " + String(outcome.why) + ".");
         }
         if (outcome.unhonoured)
         {
            LOG_APPLICATION("Resource booking: the invitation " + uid + " to " + resource->GetAddress() + " was not honoured: " + String(outcome.why) + ".");
         }

         if (!outcome.tell)
            return;

         if (outcome.leave)
         {
            // The organiser is who wrote it, so the request may be repeated.
            const AnsiString organiserText = request.organizerName.IsEmpty() ? organiser : request.organizerName + " <" + organiser + ">";
            TellDelegates(resource,
               "Booking request for " + name + ": " + ICalendar::Unescape(request.summary) + " @ " + when,
               organiserText + " asked to book " + name + ", and the request was not answered automatically: " + outcome.why + ".\r\n\r\n" +
               outcome.where + "\r\n\r\n" + Imip::Describe(request));
         }
         else
         {
            // Not honoured: nothing of the request is repeated, since whoever
            // sent it may have written it for the delegates to read.
            const AnsiString named = StringParser::IsValidEmailAddress(WideOf(organiser)) ? organiser : AnsiString("no organiser that is an address");
            TellDelegates(resource,
               "A booking request to " + name + " was not honoured",
               "A message to " + name + " carried a booking request that names " + named + " as its organiser, for " + when +
               ". It was not filed into the calendar" + (outcome.reply ? AnsiString(", and was declined") : AnsiString(", and was not answered")) +
               ": " + outcome.why + ".\r\n\r\nThe message is in the mailbox of " + name + ". You are told of requests like this at most once an hour for each organiser.\r\n");
         }
      }

      void HandleCancel(const BookingJob &job)
      {
         std::shared_ptr<const Account> resource = job.resource;
         const ImipMessage &cancel = job.message;
         if (cancel.uid.IsEmpty())
            return;

         const String uid = WideOf(cancel.uid);
         const AnsiString organiser = Normalised(WideOf(cancel.organizer));

         std::shared_ptr<std::recursive_mutex> lock = LockOf(resource->GetID());
         std::lock_guard<std::recursive_mutex> guard(*lock);

         BookingPolicy policy;
         if (!ResourceBooking::GetPolicy(resource->GetID(), policy))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7430, "ResourceBooking::HandleCancel",
               "The cancellation of " + uid + " for " + resource->GetAddress() + " was not filed because its booking policy could not be read.");
            return;
         }

         HeldBooking held;
         if (!FindHeld(resource->GetID(), uid, held))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7431, "ResourceBooking::HandleCancel",
               "The cancellation of " + uid + " for " + resource->GetAddress() + " was not filed because its calendar could not be read.");
            return;
         }
         if (!held.found)
         {
            LOG_DEBUG("Resource booking: a CANCEL for " + uid + " to " + resource->GetAddress() + " names no booking its calendar holds.");
            return;
         }

         // Only the booking's organiser cancels it, and only when the message
         // is theirs by what the server recorded: a UID is mailed to every
         // guest, and a From is whatever a sender writes. An organiser from
         // outside need not still be among the bookers to cancel what the room
         // holds of theirs.
         AnsiString whyNot;
         if (held.organiser != organiser || !IsAuthentic(policy, organiser, job.origin, whyNot, false))
         {
            LOG_APPLICATION("Resource booking: a CANCEL for " + uid + " to " + resource->GetAddress() +
               " is not from the booking's organiser, and the booking stands" + (whyNot.IsEmpty() ? String(_T(".")) : String(_T(": ")) + String(whyNot) + _T(".")));
            return;
         }
         // Older than the series the calendar holds, when it holds one; a
         // booking of occurrences alone is compared occurrence by occurrence
         // below.
         if (HasMaster(held.tree) && held.sequence > cancel.sequence)
         {
            LOG_APPLICATION("Resource booking: a CANCEL for " + uid + " to " + resource->GetAddress() +
               " is older than the booking the calendar holds, and the booking stands.");
            return;
         }

         // The whole booking, or the occurrences the CANCEL names: an event
         // of a CANCEL with a RECURRENCE-ID is that one occurrence (RFC 5546
         // section 3.2.5), taken out of a series with an EXDATE, and any
         // change of that occurrence the calendar held goes with it. A booking
         // of occurrences alone - no series held - loses the ones named and
         // keeps the rest.
         ICalComponent tree = held.tree;
         AdoptTimeZones(tree, cancel.tree);
         const ICalComponent *heldMaster = MutableMasterOf(tree);
         const bool hasMaster = heldMaster != nullptr;
         const bool recurring = heldMaster && (heldMaster->Property("RRULE") || heldMaster->Property("RDATE"));

         std::vector<ICalProperty> occurrences;
         std::vector<__int64> namedInstants;   // every occurrence the CANCEL names, placed
         bool namesWhole = false;
         bool namedNewer = false;              // one it names is held above it and above its series
         for (size_t i = 0; i < cancel.tree.children.size(); i++)
         {
            const ICalComponent &component = cancel.tree.children[i];
            if (component.name != "VEVENT")
               continue;
            const ICalProperty *recurrenceIdProperty = component.Property("RECURRENCE-ID");
            if (!recurrenceIdProperty)
            {
               namesWhole = true;
               continue;
            }

            __int64 named = 0;
            const bool placed = RecurrenceIdOf(component, cancel.tree, named);
            if (placed)
               namedInstants.push_back(named);

            // Without a series, an occurrence the calendar holds at a higher
            // SEQUENCE than this CANCEL names is newer, and stands. For a
            // CANCEL of the whole booking it is newer than the CANCEL only
            // when it is also held higher than the CANCEL's own series: a
            // CANCEL at 5 naming the occurrence at 0 still cancels it.
            if (!hasMaster)
            {
               bool newer = false;
               if (placed)
               {
                  for (size_t h = 0; h < tree.children.size() && !newer; h++)
                  {
                     __int64 instant = 0;
                     if (IsEventLike(tree.children[h]) && RecurrenceIdOf(tree.children[h], tree, instant) && instant == named)
                     {
                        const int heldSequence = SequenceOf(tree.children[h]);
                        newer = heldSequence > SequenceOf(component);
                        if (heldSequence > std::max(SequenceOf(component), static_cast<int>(cancel.sequence)))
                           namedNewer = true;
                     }
                  }
               }
               if (newer)
                  continue;
            }
            occurrences.push_back(*recurrenceIdProperty);
         }

         // Without a series, a CANCEL of the whole booking is older than it
         // when an occurrence it names is held at a higher SEQUENCE than both
         // the CANCEL's version of it and the CANCEL's series (above), or when
         // an occurrence it does not name is held at a higher SEQUENCE than
         // the CANCEL's series. An occurrence it names is never refused on the
         // series SEQUENCE alone.
         if (!hasMaster && namesWhole)
         {
            int highest = 0;
            for (size_t h = 0; h < tree.children.size(); h++)
            {
               const ICalComponent &kept = tree.children[h];
               if (!IsEventLike(kept))
                  continue;
               __int64 instant = 0;
               bool isNamed = false;
               if (RecurrenceIdOf(kept, tree, instant))
               {
                  for (size_t n = 0; n < namedInstants.size() && !isNamed; n++)
                     isNamed = namedInstants[n] == instant;
               }
               if (!isNamed && SequenceOf(kept) > highest)
                  highest = SequenceOf(kept);
            }
            if (namedNewer || highest > cancel.sequence)
            {
               LOG_APPLICATION("Resource booking: a CANCEL for " + uid + " to " + resource->GetAddress() +
                  " is older than an occurrence the calendar holds, and the booking stands.");
               return;
            }
         }
         if (!hasMaster && !namesWhole && occurrences.empty())
         {
            LOG_APPLICATION("Resource booking: a CANCEL for " + uid + " to " + resource->GetAddress() +
               " is older than the occurrences the calendar holds, and they stand.");
            return;
         }
         const bool whole = namesWhole || occurrences.empty() || (hasMaster && !recurring);

         if (whole)
         {
            for (size_t i = 0; i < tree.children.size(); i++)
            {
               ICalComponent &component = tree.children[i];
               if (!IsEventLike(component))
                  continue;
               bool had = false;
               for (size_t p = 0; p < component.properties.size(); p++)
               {
                  if (component.properties[p].name == "STATUS")
                  {
                     component.properties[p].value = "CANCELLED";
                     had = true;
                  }
               }
               if (!had)
               {
                  ICalProperty status;
                  status.name = "STATUS";
                  status.value = "CANCELLED";
                  component.properties.push_back(status);
               }
            }
         }
         else
         {
            for (size_t o = 0; o < occurrences.size(); o++)
            {
               ICalTime cancelled;
               AnsiString ignored;
               if (!ICalendar::ParseTime(occurrences[o], &cancel.tree, cancelled, ignored))
                  continue;

               RemoveOverrides(tree, cancelled.instant);

               // A series gets the EXDATE; the master may have moved in the
               // vector, so it is found again.
               ICalComponent *master = MutableMasterOf(tree);
               if (!master)
                  continue;

               ICalProperty exdate = occurrences[o];
               exdate.name = "EXDATE";
               for (size_t p = 0; p < exdate.parameters.size();)
               {
                  if (exdate.parameters[p].first == "RANGE")
                     exdate.parameters.erase(exdate.parameters.begin() + p);
                  else
                     p++;
               }
               master->properties.push_back(exdate);
            }
         }

         AnsiString problem;
         bool written = false;
         const bool gone = EventCount(tree) == 0;
         if (gone)
         {
            __int64 token = 0;
            written = CalendarStore::Delete(resource->GetID(), held.calendar.id, held.record.id, token);
            problem = "the calendar could not be written";
         }
         else
            written = WriteTree(resource->GetID(), held, tree, problem);
         if (!written)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7434, "ResourceBooking::HandleCancel",
               "The cancellation of " + uid + " for " + resource->GetAddress() + " could not be filed: " + String(problem) + ". The slot is still shown as booked.");
            return;
         }

         // The organiser cancelled it: nothing is answered about it any more
         // until they send it again and it is accepted again.
         if (gone || whole)
            DropDecision(resource->GetID(), uid);

         LOG_APPLICATION("Resource booking: " + String(whole ? _T("the booking ") : _T("occurrences of the booking ")) + uid + " of " + resource->GetAddress() +
            " were cancelled by its organiser, and the time is free again.");
      }

      void RunJob(const BookingJob &job)
      {
         try
         {
            if (job.message.method == "REQUEST")
               HandleRequest(job);
            else if (job.message.method == "CANCEL")
               HandleCancel(job);
         }
         catch (const boost::thread_interrupted &)
         {
            throw;
         }
         catch (...)
         {
            // Delivery has happened; nothing here may undo it.
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7434, "ResourceBooking::RunJob",
               "An invitation or a cancellation to " + job.resource->GetAddress() + " could not be decided, and is in its mailbox for its delegates.");
         }
      }

      // ------------------------------------------------------------------
      // The queue: hm_bookingqueue, what was delivered to each resource and is
      // not decided yet, in the order it arrived. One drain at a time per
      // resource, on a work queue of the resource booking's own
      // (ResourceBooking::WorkQueueName, DrainThreads threads), deciding one
      // delivery a task and posting itself again while more wait - so the
      // rooms take turns, and no drain holds a thread the SMTP finalisation
      // waits for. A delivery stays in the table until it has been decided,
      // so a stop, a Reinitialize or a crash puts the decision off rather
      // than losing it: the sweep every minute (WakeQueued, from
      // ScheduledMailTask) wakes every resource with one still waiting. A
      // delivery a stop interrupted half way is decided again, whole.
      // ------------------------------------------------------------------

      std::mutex queuesGuard;
      std::set<__int64> draining;

      void ReleaseDrain(__int64 accountId)
      {
         std::lock_guard<std::mutex> guard(queuesGuard);
         draining.erase(accountId);
      }

      // Whether anything waits for the resource; false when the table could
      // not be read, which the sweep answers later.
      bool AnyQueued(__int64 accountId)
      {
         SQLCommand command("select count(*) as waiting from hm_bookingqueue where bqaccountid = @ACCOUNTID");
         command.AddParameter("@ACCOUNTID", accountId);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         return recordset && !recordset->IsEOF() && recordset->GetLongValue("waiting") > 0;
      }

      // A delivery written to the queue: the origin the queue message carried
      // and the calendar part as it arrived. A value longer than its column
      // is not written rather than refused by the database.
      bool QueueDelivery(const BookingJob &job)
      {
         String sender = LowerWide(job.origin.envelopeSender);
         if (sender.GetLength() > 255)
            sender = sender.Mid(0, 255);
         String authDomains = WideOf(job.origin.authDomains);
         if (authDomains.GetLength() > 255)
            authDomains = _T("");

         SQLStatement statement;
         statement.SetTable("hm_bookingqueue");
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("bqid");
         statement.AddColumnInt64("bqaccountid", job.resource->GetID());
         statement.AddColumnInt64("bqqueued", static_cast<__int64>(time(nullptr)));
         statement.AddColumn("bqsender", sender);
         statement.AddColumnInt64("bqsubmitterid", job.origin.submitterAccountId);
         statement.AddColumn("bqauthdomains", authDomains);
         statement.AddColumnInt64("bqautosubmitted", job.origin.autoSubmitted ? 1 : 0);
         statement.AddColumn("bqcalendar", WideOf(job.message.text));
         return Application::Instance()->GetDBManager()->Execute(statement);
      }

      // One queued delivery read back as a job. readable is false for a row
      // whose calendar part no longer reads, or whose account is gone or is a
      // person now: there is nothing to decide, and the row goes.
      bool ReadQueued(__int64 id, __int64 accountId, BookingJob &job, bool &readable)
      {
         readable = false;
         SQLCommand command("select bqsender, bqsubmitterid, bqauthdomains, bqautosubmitted, bqcalendar from hm_bookingqueue where bqid = @ID");
         command.AddParameter("@ID", id);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return false;
         if (recordset->IsEOF())
            return true;

         job.resource = CacheContainer::Instance()->GetAccount(accountId);
         job.origin.envelopeSender = recordset->GetStringValue("bqsender");
         job.origin.submitterAccountId = recordset->GetInt64Value("bqsubmitterid");
         job.origin.authDomains = Utf8Of(recordset->GetStringValue("bqauthdomains"));
         job.origin.authDomains.ToLower();
         job.origin.autoSubmitted = recordset->GetLongValue("bqautosubmitted") != 0;

         AnsiString problem;
         readable = job.resource && job.resource->IsResource() &&
            Imip::Read(Utf8Of(recordset->GetStringValue("bqcalendar")), job.message, problem);
         return true;
      }

      bool Unqueue(__int64 id)
      {
         SQLCommand command("delete from hm_bookingqueue where bqid = @ID");
         command.AddParameter("@ID", id);
         return Application::Instance()->GetDBManager()->Execute(command);
      }

      void WakeDrain(__int64 accountId);

      // A delivery tried MaximumAttempts times without a decision - each try
      // counted before it was made, so a try that took the service down counts
      // too - taken off the queue, so that it neither holds its resource's
      // queue for ever nor runs again after every restart. The error log says
      // so, and the delegates are told once an hour, with nothing of it. False
      // when the row could not be taken off: it stays at the head of the
      // resource's queue, and the next minute's sweep tries again.
      bool GiveUp(__int64 accountId, __int64 id)
      {
         std::shared_ptr<const Account> resource = CacheContainer::Instance()->GetAccount(accountId);
         const String address = resource ? resource->GetAddress() : String(IntText(accountId));

         const bool removed = Unqueue(id);
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7434, "ResourceBooking::DecideOne",
            "A delivery to " + address + " could not be decided in " + String(IntText(ResourceBooking::MaximumAttempts)) + " attempts" +
            (removed ? String(_T(" and was taken off the resource booking queue; it is in the resource's mailbox for its delegates."))
                     : String(_T(", and could not be taken off the resource booking queue either."))));

         if (!removed || !resource || !resource->IsResource())
            return removed;

         bool tell = false;
         {
            std::shared_ptr<std::recursive_mutex> lock = LockOf(accountId);
            std::lock_guard<std::recursive_mutex> guard(*lock);
            // UnverifiedKeyOf's prefix alone: a key no organiser's can be
            // (none starts with a space), and none UnverifiedKeyOf makes (it
            // always puts a key after the prefix).
            tell = MayNotice(accountId, _T(" ?"), static_cast<__int64>(time(nullptr)));
         }
         if (!tell)
            return true;

         const AnsiString name = DisplayNameOf(*resource);
         TellDelegates(resource,
            "A booking request to " + name + " could not be decided",
            "A message to " + name + " carried a booking request that could not be decided in " + IntText(ResourceBooking::MaximumAttempts) +
            " attempts, and it was not answered. It is in the mailbox of " + name +
            "; answer its organiser yourself if it needs an answer. You are told of requests like this at most once an hour.\r\n");
         return true;
      }

      // The oldest delivery waiting for the resource decided, and the drain
      // posted again while another waits behind it.
      void DecideOne(__int64 accountId)
      {
         boost::this_thread::interruption_point();

         // The oldest waiting and how many wait, read as two aggregates rather
         // than every row of a backlog.
         SQLCommand head("select min(bqid) as firstid, count(*) as waiting from hm_bookingqueue where bqaccountid = @ACCOUNTID");
         head.AddParameter("@ACCOUNTID", accountId);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(head);
         const long waiting = (recordset && !recordset->IsEOF()) ? recordset->GetLongValue("waiting") : -1;
         if (waiting <= 0)
         {
            // Nothing left, or nothing readable now. Released, and asked once
            // more: a delivery queued between the read and the release found
            // this drain still running and did not post one of its own.
            ReleaseDrain(accountId);
            if (waiting == 0 && AnyQueued(accountId))
               WakeDrain(accountId);
            return;
         }

         const __int64 id = recordset->GetInt64Value("firstid");
         const bool more = waiting > 1;

         // The try counted before it is made.
         SQLCommand tries("select bqattempts from hm_bookingqueue where bqid = @ID");
         tries.AddParameter("@ID", id);
         std::shared_ptr<DALRecordset> tried = Application::Instance()->GetDBManager()->OpenRecordset(tries);
         if (!tried)
         {
            ReleaseDrain(accountId);
            return;
         }
         const long attempts = tried->IsEOF() ? 0 : tried->GetLongValue("bqattempts");
         if (attempts >= ResourceBooking::MaximumAttempts)
         {
            // A row that could not be taken off is still the head: this drain
            // stops rather than posting itself again straight onto it, and the
            // next minute's sweep tries once more.
            const bool removed = GiveUp(accountId, id);
            ReleaseDrain(accountId);
            if (removed && more)
               WakeDrain(accountId);
            return;
         }

         SQLCommand count("update hm_bookingqueue set bqattempts = bqattempts + 1 where bqid = @ID");
         count.AddParameter("@ID", id);
         if (!Application::Instance()->GetDBManager()->Execute(count))
         {
            ReleaseDrain(accountId);
            return;
         }

         BookingJob job;
         bool readable = false;
         if (!ReadQueued(id, accountId, job, readable))
         {
            // Nothing was tried: the try is given back, so that three passing
            // read errors do not give up a request never decided. (A crash in
            // the read still counts, since this line is never reached.) The
            // sweep tries again in a minute.
            SQLCommand giveBack("update hm_bookingqueue set bqattempts = bqattempts - 1 where bqid = @ID and bqattempts > 0");
            giveBack.AddParameter("@ID", id);
            Application::Instance()->GetDBManager()->Execute(giveBack);
            ReleaseDrain(accountId);
            return;
         }

         if (readable)
            RunJob(job);
         else
            LOG_APPLICATION("Resource booking: a queued delivery to account " + String(IntText(accountId)) + " had nothing left to decide - its calendar part no longer reads, or the account is gone or is a person now - and was dropped.");

         if (!Unqueue(id))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7434, "ResourceBooking::DecideOne",
               "A decided delivery could not be taken off the resource booking queue, and will be decided again.");
            ReleaseDrain(accountId);
            return;
         }

         ReleaseDrain(accountId);
         if (more || AnyQueued(accountId))
            WakeDrain(accountId);
      }

      // The ATTENDEE of the object's master that is the resource - its address
      // or an alias domain's spelling of it - as the object writes it; "" when
      // none is.
      AnsiString ResourceAttendeeOf(const ICalComponent &calendar, const Account &resource)
      {
         const ICalComponent *master = nullptr;
         for (size_t i = 0; i < calendar.children.size() && !master; i++)
         {
            if (IsEventLike(calendar.children[i]) && !calendar.children[i].Property("RECURRENCE-ID"))
               master = &calendar.children[i];
         }
         if (!master)
            return "";

         const AnsiString me = Utf8Of(LowerWide(resource.GetAddress()));
         for (size_t p = 0; p < master->properties.size(); p++)
         {
            const ICalProperty &property = master->properties[p];
            if (property.name != "ATTENDEE")
               continue;
            const AnsiString address = Imip::AddressOf(property.value);
            if (address == me || Normalised(WideOf(address)) == me)
               return address;
         }
         return "";
      }

      class BookingDrain : public Task
      {
      public:
         explicit BookingDrain(__int64 accountId) :
            Task(_T("Resource booking")),
            accountId_(accountId),
            ran_(false)
         {
         }

         virtual ~BookingDrain()
         {
            // Destroyed without having run: its queue was stopping. What waits
            // stays in the table for the next start's sweep.
            if (ran_)
               return;
            try
            {
               ReleaseDrain(accountId_);
            }
            catch (...)
            {
            }
         }

         virtual void DoWork()
         {
            ran_ = true;
            try
            {
               DecideOne(accountId_);
            }
            catch (const boost::thread_interrupted &)
            {
               ReleaseDrain(accountId_);
               throw;
            }
            catch (...)
            {
               // Never left holding the resource: a drain that died would
               // otherwise keep every later delivery to it from posting one.
               ReleaseDrain(accountId_);
               LOG_APPLICATION("Resource booking: a decision for account " + String(IntText(accountId_)) +
                  " did not finish; what waits is decided by the next sweep.");
            }
         }

      private:
         __int64 accountId_;
         bool ran_;
      };

      // A drain posted for the resource unless one is already running or
      // waiting. With no queue to post to - the service starting or stopping -
      // nothing is posted, and the sweep wakes it later.
      void WakeDrain(__int64 accountId)
      {
         {
            std::lock_guard<std::mutex> guard(queuesGuard);
            if (!draining.insert(accountId).second)
               return;
         }

         std::shared_ptr<WorkQueue> queue = WorkQueueManager::Instance()->GetQueue(ResourceBooking::WorkQueueName());
         if (!queue)
         {
            ReleaseDrain(accountId);
            return;
         }

         queue->AddTask(std::shared_ptr<Task>(new BookingDrain(accountId)));
      }
   }

   AnsiString
   ResourceBooking::KindWord(int kind)
   {
      if (kind == Account::KindRoom)
         return "room";
      if (kind == Account::KindEquipment)
         return "equipment";
      return "person";
   }

   bool
   ResourceBooking::ReadKind(const String &word, int &kind)
   {
      const String lower = LowerWide(word);
      if (lower == _T("person") || lower == _T("0"))
         kind = Account::KindPerson;
      else if (lower == _T("room") || lower == _T("1"))
         kind = Account::KindRoom;
      else if (lower == _T("equipment") || lower == _T("2"))
         kind = Account::KindEquipment;
      else
         return false;
      return true;
   }

   std::vector<String>
   ResourceBooking::SplitList(const String &list)
   {
      std::vector<String> entries;
      String current;
      for (int i = 0; i <= list.GetLength(); i++)
      {
         const wchar_t c = i < list.GetLength() ? list[i] : L',';
         if (c == L',' || c == L';' || c == L'\n' || c == L'\r')
         {
            String entry = TrimmedWide(current);
            if (!entry.IsEmpty())
               entries.push_back(entry);
            current = _T("");
            continue;
         }
         current += c;
      }
      return entries;
   }

   String
   ResourceBooking::JoinList(const std::vector<String> &entries)
   {
      String joined;
      for (size_t i = 0; i < entries.size(); i++)
      {
         if (i > 0)
            joined += _T(", ");
         joined += entries[i];
      }
      return joined;
   }

   bool
   ResourceBooking::GetPolicy(__int64 accountId, BookingPolicy &policy)
   {
      policy = BookingPolicy();
      policy.accountId = accountId;

      SQLCommand command(String(AnsiString("select ") + PolicyColumns + " from hm_bookingpolicies where bpaccountid = @ACCOUNTID"));
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      if (recordset->IsEOF())
         return true;

      policy.autoAccept = recordset->GetLongValue("bpautoaccept") != 0;
      policy.windowDays = static_cast<int>(recordset->GetLongValue("bpwindowdays"));
      policy.maxMinutes = static_cast<int>(recordset->GetLongValue("bpmaxminutes"));
      policy.workingHoursOnly = recordset->GetLongValue("bpworkinghoursonly") != 0;
      policy.workDays = static_cast<int>(recordset->GetLongValue("bpworkdays"));
      policy.workStart = static_cast<int>(recordset->GetLongValue("bpworkstart"));
      policy.workEnd = static_cast<int>(recordset->GetLongValue("bpworkend"));
      policy.timeZone = recordset->GetStringValue("bptimezone");
      policy.allowConflicts = recordset->GetLongValue("bpallowconflicts") != 0;
      policy.allowRecurring = recordset->GetLongValue("bpallowrecurring") != 0;
      policy.bookers = recordset->GetStringValue("bpbookers");
      policy.capacity = static_cast<int>(recordset->GetLongValue("bpcapacity"));
      policy.stored = true;

      // A row a hand or an older build wrote badly is read as the nearest
      // policy this server keeps, not refused: a room with a broken row must
      // still answer.
      if (policy.windowDays < 1 || policy.windowDays > MaximumWindowDays)
         policy.windowDays = 180;
      if (policy.maxMinutes < 0)
         policy.maxMinutes = 0;
      if (policy.timeZone.IsEmpty())
         policy.timeZone = _T("UTC");
      return true;
   }

   bool
   ResourceBooking::SavePolicy(const BookingPolicy &policy)
   {
      if (policy.accountId <= 0)
         return false;

      BookingPolicy current;
      if (!GetPolicy(policy.accountId, current))
         return false;

      SQLStatement statement;
      statement.SetTable("hm_bookingpolicies");

      if (current.stored)
      {
         AnsiString where;
         where.Format("bpaccountid = %I64d", policy.accountId);
         statement.SetStatementType(SQLStatement::STUpdate);
         statement.SetWhereClause(String(where));
      }
      else
      {
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("bpid");
         statement.AddColumnInt64("bpaccountid", policy.accountId);
      }

      statement.AddColumnInt64("bpautoaccept", policy.autoAccept ? 1 : 0);
      statement.AddColumnInt64("bpwindowdays", policy.windowDays);
      statement.AddColumnInt64("bpmaxminutes", policy.maxMinutes);
      statement.AddColumnInt64("bpworkinghoursonly", policy.workingHoursOnly ? 1 : 0);
      statement.AddColumnInt64("bpworkdays", policy.workDays);
      statement.AddColumnInt64("bpworkstart", policy.workStart);
      statement.AddColumnInt64("bpworkend", policy.workEnd);
      statement.AddColumn("bptimezone", policy.timeZone);
      statement.AddColumnInt64("bpallowconflicts", policy.allowConflicts ? 1 : 0);
      statement.AddColumnInt64("bpallowrecurring", policy.allowRecurring ? 1 : 0);
      statement.AddColumn("bpbookers", policy.bookers);
      statement.AddColumnInt64("bpcapacity", policy.capacity);

      return Application::Instance()->GetDBManager()->Execute(statement);
   }

   bool
   ResourceBooking::CheckPolicy(BookingPolicy &policy, const String &previousBookers, bool mayNameGroups, String &problem, bool &outside)
   {
      problem = _T("");
      outside = false;

      if (policy.windowDays < 1 || policy.windowDays > MaximumWindowDays)
      {
         problem.Format(_T("window_days is a whole number of days from 1 to %d"), MaximumWindowDays);
         return false;
      }
      if (policy.maxMinutes < 0 || policy.maxMinutes > MaximumDurationMinutes)
      {
         problem.Format(_T("max_duration_minutes is a whole number of minutes from 0 (no limit) to %d"), MaximumDurationMinutes);
         return false;
      }
      if (policy.workDays < 1 || policy.workDays > 127)
      {
         problem = _T("work_days names at least one day of the week: mon, tue, wed, thu, fri, sat or sun");
         return false;
      }
      if (policy.workStart < 0 || policy.workStart > 1439 || policy.workEnd < 1 || policy.workEnd > 1440 || policy.workStart >= policy.workEnd)
      {
         problem = _T("work_start and work_end are times of day as HH:MM, work_end after work_start and no later than 24:00");
         return false;
      }
      if (policy.capacity < 0 || policy.capacity > MaximumCapacity)
      {
         problem.Format(_T("capacity is a whole number of people from 0 (not said) to %d"), MaximumCapacity);
         return false;
      }

      policy.timeZone = TrimmedWide(policy.timeZone);
      if (policy.timeZone.IsEmpty() || policy.timeZone.GetLength() > MaximumTimeZoneLength || !ICalendar::KnowsTimeZone(Utf8Of(policy.timeZone)))
      {
         problem = _T("time_zone is a time zone this server knows, such as Europe/London, America/New_York or UTC");
         return false;
      }

      // The entries already stored, by their lower case, as they are stored:
      // a group among them is kept as it is, whether or not it still exists.
      std::map<String, String> stored;
      const std::vector<String> previous = SplitList(previousBookers);
      for (size_t i = 0; i < previous.size(); i++)
         stored[LowerWide(previous[i])] = previous[i];

      std::vector<String> entries = SplitList(policy.bookers);
      std::vector<String> kept;
      std::set<String> seen;
      for (size_t i = 0; i < entries.size(); i++)
      {
         String entry = entries[i];

         if (entry.StartsWith(_T("@")))
         {
            entry.ToLower();
            const String domain = entry.Mid(1);
            if (domain.IsEmpty() || domain.Find(_T("@")) >= 0 || domain.Find(_T(".")) <= 0 || domain.Find(_T(" ")) >= 0)
            {
               problem = entries[i] + _T(" is not a domain: a domain is written @example.com");
               return false;
            }
         }
         else if (entry.Find(_T("@")) >= 0)
         {
            entry.ToLower();
            if (!StringParser::IsValidEmailAddress(entry))
            {
               problem = entries[i] + _T(" is not an e-mail address");
               return false;
            }
         }
         else
         {
            std::map<String, String>::const_iterator known = stored.find(LowerWide(entry));
            if (known != stored.end())
               entry = known->second;
            else if (!mayNameGroups)
            {
               // Before any lookup: a caller confined to named domains learns
               // nothing of the server's groups from the answer.
               problem = entries[i] + _T(" is not an address or an @domain, and a credential confined to named domains names bookers by address and @domain only");
               outside = true;
               return false;
            }
            else
            {
               String canonical;
               if (GroupIdOf(entry, canonical) == 0)
               {
                  problem = entries[i] + _T(" is neither an address, an @domain nor the name of a group of this server");
                  return false;
               }
               entry = canonical;
            }
         }

         if (!seen.insert(LowerWide(entry)).second)
            continue;
         kept.push_back(entry);
      }

      policy.bookers = JoinList(kept);
      if (policy.bookers.GetLength() > MaximumBookersLength)
      {
         problem.Format(_T("bookers is at most %d characters written out"), MaximumBookersLength);
         return false;
      }

      return true;
   }

   bool
   ResourceBooking::DelegatesOf(__int64 accountId, std::vector<String> &addresses)
   {
      addresses.clear();

      CalendarRecord calendar;
      if (!CalendarStore::DefaultIfAny(accountId, calendar))
         return true;

      std::vector<CalendarShareRecord> shares;
      if (!CalendarStore::ListShares(calendar.id, shares))
         return false;

      std::set<String> seen;
      for (size_t i = 0; i < shares.size(); i++)
      {
         if (!shares[i].delegate)
            continue;

         if (shares[i].accountId > 0)
         {
            String address = shares[i].address;
            address.ToLower();
            if (!address.IsEmpty() && seen.insert(address).second)
               addresses.push_back(address);
            continue;
         }

         SQLCommand members("select accountaddress from hm_group_members inner join hm_accounts on accountid = memberaccountid "
                            "where membergroupid = @GROUPID");
         members.AddParameter("@GROUPID", shares[i].groupId);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(members);
         if (!recordset)
            return false;
         while (!recordset->IsEOF())
         {
            String address = recordset->GetStringValue("accountaddress");
            address.ToLower();
            if (!address.IsEmpty() && seen.insert(address).second)
               addresses.push_back(address);
            recordset->MoveNext();
         }
      }

      return true;
   }

   bool
   ResourceBooking::DelegateEntries(__int64 accountId, std::vector<String> &entries)
   {
      entries.clear();

      CalendarRecord calendar;
      if (!CalendarStore::DefaultIfAny(accountId, calendar))
         return true;

      std::vector<CalendarShareRecord> shares;
      if (!CalendarStore::ListShares(calendar.id, shares))
         return false;

      for (size_t i = 0; i < shares.size(); i++)
      {
         if (!shares[i].delegate)
            continue;
         if (shares[i].accountId > 0)
         {
            String address = shares[i].address;
            address.ToLower();
            entries.push_back(address);
         }
         else
            entries.push_back(shares[i].groupName);
      }

      return true;
   }

   bool
   ResourceBooking::ResolveDelegates(std::shared_ptr<const Account> resource, const std::vector<String> &entries, const std::vector<String> &allowedDomains,
                                     std::vector<__int64> &accountIds, std::vector<__int64> &groupIds, String &problem, bool &outside)
   {
      accountIds.clear();
      groupIds.clear();
      problem = _T("");
      outside = false;
      if (!resource)
         return false;

      for (size_t i = 0; i < entries.size(); i++)
      {
         const String entry = TrimmedWide(entries[i]);
         if (entry.IsEmpty())
            continue;

         if (entry.Find(_T("@")) > 0)
         {
            const String address = WideOf(Normalised(entry));

            // The confinement first, before the address is looked up: a
            // refusal must not tell a caller confined to one domain which
            // addresses another domain has.
            if (!allowedDomains.empty())
            {
               const String domain = WideOf(DomainPart(Utf8Of(address)));
               bool allowed = false;
               for (size_t d = 0; d < allowedDomains.size() && !allowed; d++)
                  allowed = allowedDomains[d].CompareNoCase(domain) == 0;
               if (!allowed)
               {
                  problem = entry + _T(" is outside the domains this credential may manage");
                  outside = true;
                  return false;
               }
            }

            std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(address);
            if (!account)
            {
               problem = entry + _T(" is not an account of this server");
               return false;
            }
            if (account->GetID() == resource->GetID())
            {
               problem = _T("a room or a piece of equipment is not its own delegate");
               return false;
            }
            if (account->IsResource())
            {
               problem = entry + _T(" is a room or a piece of equipment, and cannot be signed into to answer for another");
               return false;
            }
            if (std::find(accountIds.begin(), accountIds.end(), account->GetID()) == accountIds.end())
               accountIds.push_back(account->GetID());
            continue;
         }

         // A group's members may be in any domain.
         if (!allowedDomains.empty())
         {
            problem = entry + _T(" is a group, and a credential confined to named domains names delegates by address");
            outside = true;
            return false;
         }

         String canonical;
         const __int64 groupId = GroupIdOf(entry, canonical);
         if (groupId == 0)
         {
            problem = entry + _T(" is neither an account nor a group of this server");
            return false;
         }
         if (std::find(groupIds.begin(), groupIds.end(), groupId) == groupIds.end())
            groupIds.push_back(groupId);
      }

      return true;
   }

   bool
   ResourceBooking::ApplyDelegates(std::shared_ptr<const Account> resource, const std::vector<__int64> &accountIds, const std::vector<__int64> &groupIds, String &problem)
   {
      problem = _T("");
      if (!resource)
         return false;

      // Taken with the decisions' lock, so a share never changes in the middle
      // of a decision about who is told.
      ResourceCalendarGuard guard(resource->GetID());

      CalendarRecord calendar;
      if (!CalendarStore::EnsureDefault(resource->GetID(), calendar))
      {
         problem = _T("the calendar could not be read");
         return false;
      }

      std::vector<CalendarShareRecord> shares;
      if (!CalendarStore::ListShares(calendar.id, shares))
      {
         problem = _T("the calendar's shares could not be read");
         return false;
      }

      bool written = true;
      for (size_t i = 0; i < shares.size(); i++)
      {
         if (!shares[i].delegate)
            continue;

         if (shares[i].accountId > 0 && std::find(accountIds.begin(), accountIds.end(), shares[i].accountId) == accountIds.end())
            written = CalendarStore::Unshare(calendar.id, shares[i].accountId) && written;
         else if (shares[i].groupId > 0 && std::find(groupIds.begin(), groupIds.end(), shares[i].groupId) == groupIds.end())
            written = CalendarStore::UnshareGroup(calendar.id, shares[i].groupId) && written;
      }

      for (size_t i = 0; i < accountIds.size(); i++)
         written = CalendarStore::Share(calendar.id, accountIds[i], 0, true, true) && written;
      for (size_t i = 0; i < groupIds.size(); i++)
         written = CalendarStore::Share(calendar.id, 0, groupIds[i], true, true) && written;

      if (!written)
         problem = _T("the calendar's shares could not all be written");
      return written;
   }

   void
   ResourceBooking::OnDelivery(std::shared_ptr<const Account> resource, const ImipMessage &message, const String &fileName,
                               std::shared_ptr<const Message> original)
   {
      if (!resource)
         return;

      try
      {
         if (message.uid.IsEmpty() || (message.method != "REQUEST" && message.method != "CANCEL"))
            return;

         BookingJob job;
         job.resource = resource;
         job.message = message;
         if (original)
         {
            job.origin.envelopeSender = original->GetFromAddress();
            // A message an application sent through the send API records the
            // account its key sent as, for the domain rules; no session of that
            // account wrote it (Message::FlagSubmittedThroughSendApi), and an
            // API key is no organiser's session.
            job.origin.submitterAccountId = original->GetSubmittedThroughSendApi() ? 0 : original->GetSubmitterAccountID();
            job.origin.authDomains = Utf8Of(original->GetAuthDomains());
            job.origin.authDomains.ToLower();
            // DMARC vouches for the domain of the From, and the From is the
            // address that domain let be written: the pass vouches for the
            // organiser the message names only when the From is that organiser
            // (Imip::VouchedFromOutside), and is kept only then - another
            // sender at the organiser's domain is not the organiser.
            if (!job.origin.authDomains.IsEmpty() &&
                !Imip::VouchedFromOutside(job.origin.authDomains, Imip::FromAddressOf(PersistentMessage::LoadHeader(fileName, false)), message.organizer))
               job.origin.authDomains = "";
         }
         job.origin.autoSubmitted = AutoSubmittedIn(fileName);

         // Written to the queue before anything is decided, so that nothing
         // between here and the decision can lose it.
         if (!QueueDelivery(job))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7434, "ResourceBooking::OnDelivery",
               "An invitation or a cancellation to " + resource->GetAddress() + " could not be written to the resource booking queue, so nothing will decide it; it is in the resource's mailbox.");
            return;
         }

         WakeDrain(resource->GetID());
      }
      catch (...)
      {
         // Delivery has happened; nothing here may undo it.
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7434, "ResourceBooking::OnDelivery",
            "An invitation or a cancellation to " + resource->GetAddress() + " could not be queued to be decided; it is in the resource's mailbox.");
      }
   }

   String
   ResourceBooking::WorkQueueName()
   {
      return _T("Resource booking queue");
   }

   void
   ResourceBooking::WakeQueued()
   {
      try
      {
         SQLCommand command("select distinct bqaccountid from hm_bookingqueue");
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return;

         std::vector<__int64> waiting;
         while (!recordset->IsEOF())
         {
            waiting.push_back(recordset->GetInt64Value("bqaccountid"));
            recordset->MoveNext();
         }

         for (size_t i = 0; i < waiting.size(); i++)
            WakeDrain(waiting[i]);
      }
      catch (...)
      {
         // The next minute's sweep asks again.
      }
   }

   AnsiString
   ResourceBooking::AnswerableOrganiser(const Account &resource, const String &uid, const AnsiString &organizer)
   {
      try
      {
         if (!resource.IsResource())
            return "";

         AnsiString decided;
         if (!ReadDecision(resource.GetID(), uid, decided) || decided.IsEmpty())
            return "";

         // The booking as it is now still names the organiser the resource
         // believed; one whose ORGANIZER was rewritten since is answered to
         // nobody, rather than to either.
         if (Normalised(WideOf(organizer)) != decided)
            return "";
         return decided;
      }
      catch (...)
      {
         return "";
      }
   }

   AnsiString
   ResourceBooking::AnswerableOrganiser(const Account &resource, const ICalComponent &calendar)
   {
      try
      {
         ImipMessage booking;
         AnsiString problem;
         if (!Imip::Read(ICalendar::Serialize(calendar), booking, problem))
            return "";
         return AnswerableOrganiser(resource, WideOf(booking.uid), booking.organizer);
      }
      catch (...)
      {
         return "";
      }
   }

   void
   ResourceBooking::ForgetDecision(__int64 accountId, const String &uid)
   {
      try
      {
         // Whatever the account's kind now: one that was a room, and is a
         // person for a while, keeps the rows it had as a room. The indexed
         // read first, so an account with no record reads nothing else.
         AnsiString organiser;
         if (!ReadDecision(accountId, uid, organiser) || organiser.IsEmpty())
            return;

         // Kept while another object of the account still holds the UID: a
         // CalDAV client moves an event as a PUT into one calendar and then a
         // DELETE from the other, and the booking lives on in the first.
         std::set<String> held;
         if (!HeldUidsOf(accountId, held) || held.count(uid) > 0)
            return;
         DropDecision(accountId, uid);
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7434, "ResourceBooking::ForgetDecision",
            _T("The booking record of ") + uid + _T(" could not be forgotten: an unexpected error stopped it. It is forgotten when a calendar of the account is next deleted, or with the account."));
      }
   }

   void
   ResourceBooking::ForgetGoneBookings(__int64 accountId)
   {
      try
      {
         SQLCommand command("select bduid from hm_bookingdecisions where bdaccountid = @ACCOUNTID");
         command.AddParameter("@ACCOUNTID", accountId);
         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
         if (!recordset)
            return;

         std::vector<String> uids;
         while (!recordset->IsEOF())
         {
            uids.push_back(recordset->GetStringValue("bduid"));
            recordset->MoveNext();
         }
         if (uids.empty())
            return;

         // Two reads whatever the count: the records, and what is held.
         std::set<String> held;
         if (!HeldUidsOf(accountId, held))
            return;
         for (size_t i = 0; i < uids.size(); i++)
         {
            if (held.count(uids[i]) == 0)
               DropDecision(accountId, uids[i]);
         }
      }
      catch (...)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7434, "ResourceBooking::ForgetGoneBookings",
            _T("The booking records of account ") + StringParser::IntToString(accountId) +
            _T(" whose calendars are gone could not all be forgotten: an unexpected error stopped the pass. The next calendar deleted from the account runs it again."));
      }
   }

   bool
   ResourceBooking::TellOrganiserOfDecline(std::shared_ptr<const Account> resource, std::shared_ptr<const Account> delegate, const ICalComponent &calendar,
                                           const AnsiString &organiser)
   {
      if (!resource || !delegate)
         return false;

      try
      {
         const AnsiString attendee = ResourceAttendeeOf(calendar, *resource);
         ImipMessage declined;
         AnsiString problem;
         if (attendee.IsEmpty() || !Imip::Read(ICalendar::Serialize(calendar), declined, problem))
            return false;

         // The organiser the resource recorded when it accepted the booking
         // (AnswerableOrganiser, read by the caller under the resource's lock
         // before the record went with the booking), and nobody else.
         const AnsiString resourceAddress = Utf8Of(LowerWide(resource->GetAddress()));
         if (organiser.IsEmpty() || organiser == resourceAddress)
         {
            LOG_APPLICATION("Resource booking: " + delegate->GetAddress() + " declined " + WideOf(declined.uid) + " for " + resource->GetAddress() +
               ", and nobody was told: " + resource->GetAddress() + " did not accept it from the organiser it names, or has no record of whom it accepted it from.");
            return false;
         }

         // The same answer the REST route gives for a delegate's decline: From
         // the resource, Sender the delegate, SENT-BY on the resource's ATTENDEE.
         const AnsiString name = DisplayNameOf(*resource);
         const AnsiString delegateAddress = Utf8Of(LowerWide(delegate->GetAddress()));
         const AnsiString cutype = resource->GetKind() == Account::KindRoom ? "ROOM" : "RESOURCE";
         const AnsiString replyText = Imip::ReplyObject(declined, attendee, name, "DECLINED", delegateAddress, cutype);
         const AnsiString subject = "Declined: " + ICalendar::Unescape(declined.summary) + " @ " + Imip::WhenText(declined.start, declined.end, declined.allDay);
         const AnsiString delegateName = DisplayNameOf(*delegate);
         const AnsiString body = name + " <" + resourceAddress + "> has declined this invitation, answered by " +
            delegateName + " <" + delegateAddress + "> on its behalf.\r\n\r\n" + Imip::Describe(declined);
         const AnsiString mail = Imip::BuildMail(FromHeaderOf(*resource), organiser, subject, body, "REPLY", replyText, resourceAddress, FromHeaderOf(*delegate));

         const int status = QueueMail(resource, organiser, mail, problem, delegate);
         if (status != 0)
         {
            LOG_APPLICATION("Resource booking: the organiser of " + WideOf(declined.uid) + " was not told that " + delegate->GetAddress() +
               " declined it for " + resource->GetAddress() + ": " + String(problem));
            return false;
         }
         return true;
      }
      catch (...)
      {
         return false;
      }
   }

   bool
   ResourceBooking::DeclinedBy(const ICalComponent &calendar, const Account &resource)
   {
      const ICalComponent *master = nullptr;
      for (size_t i = 0; i < calendar.children.size() && !master; i++)
      {
         if (IsEventLike(calendar.children[i]) && !calendar.children[i].Property("RECURRENCE-ID"))
            master = &calendar.children[i];
      }
      if (!master)
         return false;

      const AnsiString me = Utf8Of(LowerWide(resource.GetAddress()));
      for (size_t p = 0; p < master->properties.size(); p++)
      {
         const ICalProperty &property = master->properties[p];
         if (property.name != "ATTENDEE")
            continue;
         const AnsiString address = Imip::AddressOf(property.value);
         if (address != me && Normalised(WideOf(address)) != me)
            continue;
         AnsiString partstat = Trimmed(property.Parameter("PARTSTAT"));
         partstat.ToUpper();
         return partstat == "DECLINED";
      }
      return false;
   }

   int
   ResourceBooking::EraseAddress(const String &address)
   {
      const String wanted = LowerWide(address);
      if (wanted.IsEmpty())
         return 0;
      const AnsiString wantedResolved = Normalised(address);

      SQLCommand command("select bpaccountid, bpbookers from hm_bookingpolicies");
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return -1;

      std::vector<std::pair<__int64, String> > changed;
      while (!recordset->IsEOF())
      {
         const __int64 accountId = recordset->GetInt64Value("bpaccountid");
         const std::vector<String> entries = SplitList(recordset->GetStringValue("bpbookers"));
         std::vector<String> kept;
         for (size_t i = 0; i < entries.size(); i++)
         {
            // The entry whole, as written or resolved through an alias
            // domain: erasing ann@ never touches joann@.
            const String entry = LowerWide(entries[i]);
            const bool isAddress = !entry.StartsWith(_T("@")) && entry.Find(_T("@")) > 0;
            if (entry == wanted || (isAddress && !wantedResolved.IsEmpty() && Normalised(entry) == wantedResolved))
               continue;
            kept.push_back(entries[i]);
         }
         if (kept.size() != entries.size())
            changed.push_back(std::make_pair(accountId, JoinList(kept)));
         recordset->MoveNext();
      }

      int count = 0;
      for (size_t i = 0; i < changed.size(); i++)
      {
         // A list the erasure leaves empty would mean everyone in the
         // organisation - more than it said - so the policy's automatic
         // answers are switched off with it, and its delegates answer until
         // an administrator names who may book again.
         const bool emptied = changed[i].second.IsEmpty();
         SQLCommand update(emptied
            ? String(_T("update hm_bookingpolicies set bpbookers = @BOOKERS, bpautoaccept = 0 where bpaccountid = @ACCOUNTID"))
            : String(_T("update hm_bookingpolicies set bpbookers = @BOOKERS where bpaccountid = @ACCOUNTID")));
         update.AddParameter("@BOOKERS", changed[i].second);
         update.AddParameter("@ACCOUNTID", changed[i].first);
         if (!Application::Instance()->GetDBManager()->Execute(update))
            return -1;
         count++;
      }

      // What the address sent a resource that waits to be decided
      // (hm_bookingqueue, the sender kept lower-cased): it is not decided.
      SQLCommand queuedCount("select count(*) as c from hm_bookingqueue where bqsender = @SENDER1 or bqsender = @SENDER2");
      queuedCount.AddParameter("@SENDER1", wanted);
      queuedCount.AddParameter("@SENDER2", WideOf(wantedResolved).IsEmpty() ? wanted : WideOf(wantedResolved));
      std::shared_ptr<DALRecordset> queued = Application::Instance()->GetDBManager()->OpenRecordset(queuedCount);
      if (!queued)
         return -1;
      const int queuedRows = queued->IsEOF() ? 0 : static_cast<int>(queued->GetLongValue("c"));
      if (queuedRows > 0)
      {
         SQLCommand unqueue("delete from hm_bookingqueue where bqsender = @SENDER1 or bqsender = @SENDER2");
         unqueue.AddParameter("@SENDER1", wanted);
         unqueue.AddParameter("@SENDER2", WideOf(wantedResolved).IsEmpty() ? wanted : WideOf(wantedResolved));
         if (!Application::Instance()->GetDBManager()->Execute(unqueue))
            return -1;
         count += queuedRows;
      }

      // The resources' memory of the address as an organiser (hm_bookinganswers,
      // kept lower-cased and resolved), under its own key and under the one
      // the requests naming it that nobody verified were counted under
      // (UnverifiedKeyOf): forgotten, and answered again as one they have not
      // heard from.
      const String resolved = WideOf(wantedResolved);
      const String organiserKey = resolved.IsEmpty() ? wanted : resolved;
      SQLCommand counted("select count(*) as c from hm_bookinganswers where baorganiser = @ORGANISER1 or baorganiser = @ORGANISER2 or baorganiser = @ORGANISER3 or baorganiser = @ORGANISER4");
      counted.AddParameter("@ORGANISER1", wanted);
      counted.AddParameter("@ORGANISER2", organiserKey);
      counted.AddParameter("@ORGANISER3", UnverifiedKeyOf(wanted));
      counted.AddParameter("@ORGANISER4", UnverifiedKeyOf(organiserKey));
      std::shared_ptr<DALRecordset> remembered = Application::Instance()->GetDBManager()->OpenRecordset(counted);
      if (!remembered)
         return -1;
      const int rows = remembered->IsEOF() ? 0 : static_cast<int>(remembered->GetLongValue("c"));
      if (rows > 0)
      {
         SQLCommand forget("delete from hm_bookinganswers where baorganiser = @ORGANISER1 or baorganiser = @ORGANISER2 or baorganiser = @ORGANISER3 or baorganiser = @ORGANISER4");
         forget.AddParameter("@ORGANISER1", wanted);
         forget.AddParameter("@ORGANISER2", organiserKey);
         forget.AddParameter("@ORGANISER3", UnverifiedKeyOf(wanted));
         forget.AddParameter("@ORGANISER4", UnverifiedKeyOf(organiserKey));
         if (!Application::Instance()->GetDBManager()->Execute(forget))
            return -1;
         count += rows;
      }

      // The bookings the resources recorded they accepted from the address
      // (hm_bookingdecisions, kept lower-cased and resolved): forgotten, so a
      // delegate's answer about one tells nobody. The bookings themselves are
      // calendar objects, and go with the calendars.
      SQLCommand decidedCount("select count(*) as c from hm_bookingdecisions where bdorganiser = @ORGANISER1 or bdorganiser = @ORGANISER2");
      decidedCount.AddParameter("@ORGANISER1", wanted);
      decidedCount.AddParameter("@ORGANISER2", organiserKey);
      std::shared_ptr<DALRecordset> decided = Application::Instance()->GetDBManager()->OpenRecordset(decidedCount);
      if (!decided)
         return -1;
      const int decidedRows = decided->IsEOF() ? 0 : static_cast<int>(decided->GetLongValue("c"));
      if (decidedRows > 0)
      {
         SQLCommand undecide("delete from hm_bookingdecisions where bdorganiser = @ORGANISER1 or bdorganiser = @ORGANISER2");
         undecide.AddParameter("@ORGANISER1", wanted);
         undecide.AddParameter("@ORGANISER2", organiserKey);
         if (!Application::Instance()->GetDBManager()->Execute(undecide))
            return -1;
         count += decidedRows;
      }

      return count;
   }

   ResourceCalendarGuard::ResourceCalendarGuard(__int64 ownerAccountId)
   {
      std::shared_ptr<const Account> owner = CacheContainer::Instance()->GetAccount(ownerAccountId);
      if (!owner || !owner->IsResource())
         return;
      mutex_ = LockOf(ownerAccountId);
      mutex_->lock();
   }

   ResourceCalendarGuard::~ResourceCalendarGuard()
   {
      if (mutex_)
         mutex_->unlock();
   }

   // ------------------------------------------------------------------------
   // The in-process tests, run by ClassTester::DoTests.
   // ------------------------------------------------------------------------

   namespace
   {
      AnsiString Repeated(char c, int count)
      {
         AnsiString text;
         for (int i = 0; i < count; i++)
            text += c;
         return text;
      }

      AnsiString RecordedFor(const AnsiString &dmarcDomain)
      {
         AuthenticationResults results;
         results.SetDmarc(AuthenticationResults::ResultPass, dmarcDomain, "");
         return results.PassedDomains();
      }
   }

   void
   ResourceBookingTester::Test()
   {
      // A DMARC pass for a From domain of 253 characters - four labels of at
      // most 63, a legal name - would make a token of 259, longer than the 255
      // characters hm_messages.messageauthdomains holds, and an INSERT the
      // database refuses: the message could not be saved and the sender would
      // be answered 451 on every retry. The token is dropped whole instead, and
      // never cut into the name of another domain.
      const AnsiString label = Repeated('a', 63);
      const AnsiString longest = label + "." + label + "." + label + "." + Repeated('b', 61);
      if (longest.GetLength() != 253)
         throw std::logic_error("ResourceBookingTester: the 253-character domain was not built as 253 characters.");
      if (!RecordedFor(longest).IsEmpty())
         throw std::logic_error("ResourceBookingTester: a DMARC pass for a 253-character domain was recorded; it does not fit the column and must be dropped.");

      // 249 characters is the longest that fits: "dmarc=" and the domain are
      // exactly 255. 250 is one too many.
      const AnsiString fits = label + "." + label + "." + label + "." + Repeated('c', 57);
      const AnsiString recorded = RecordedFor(fits);
      if (recorded != "dmarc=" + fits || recorded.GetLength() != 255)
         throw std::logic_error("ResourceBookingTester: a DMARC pass for a 249-character domain was not recorded whole in 255 characters.");
      if (!RecordedFor(fits + "c").IsEmpty())
         throw std::logic_error("ResourceBookingTester: a DMARC pass for a 250-character domain was recorded; it does not fit.");

      // Lower-cased; a value that is not purely a domain's characters is left
      // out rather than cleaned into another domain.
      if (RecordedFor("Partner.EXAMPLE") != "dmarc=partner.example")
         throw std::logic_error("ResourceBookingTester: a DMARC pass was not recorded lower-cased.");
      if (!RecordedFor("part ner.example").IsEmpty() || !RecordedFor("partner.example;x").IsEmpty())
         throw std::logic_error("ResourceBookingTester: a DMARC domain with a character no domain has was recorded.");

      // Only a pass is recorded, and SPF on its own is not.
      {
         AuthenticationResults failed;
         failed.SetDmarc(AuthenticationResults::ResultFail, "partner.example", "reject");
         if (!failed.PassedDomains().IsEmpty())
            throw std::logic_error("ResourceBookingTester: a DMARC failure was recorded as a pass.");

         AuthenticationResults spfOnly;
         spfOnly.SetSpf(AuthenticationResults::ResultPass, "smtp.mailfrom", "bounce@partner.example", "192.0.2.1", "mail.partner.example");
         if (!spfOnly.PassedDomains().IsEmpty())
            throw std::logic_error("ResourceBookingTester: an SPF pass alone was recorded; it vouches for nobody's From.");
      }

      // What vouches for an organiser from outside: DMARC for exactly its
      // domain, and nothing else.
      if (!Vouched("dmarc=partner.example", "partner.example"))
         throw std::logic_error("ResourceBookingTester: a DMARC pass for the organiser's domain did not vouch for it.");
      if (Vouched("dmarc=partner.example", "example") || Vouched("dmarc=partner.example.evil", "partner.example") ||
          Vouched("dmarc=sub.partner.example", "partner.example"))
         throw std::logic_error("ResourceBookingTester: a DMARC pass for another domain vouched for the organiser's.");
      if (Vouched("spf=partner.example", "partner.example") || Vouched("", "partner.example") || Vouched("dmarc=partner.example", ""))
         throw std::logic_error("ResourceBookingTester: something other than a DMARC pass vouched for an organiser.");
   }
}
