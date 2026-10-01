// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The signed-in account's calendars over REST: what the webmail's calendar
// and tasks pages read and write. Several calendars with colours and shares -
// with a named account or with one of the server's groups, read-only,
// read-write or as a delegate - (/api/v1/me/calendars), the events of a
// window expanded instance by instance across every calendar the account can
// see (/api/v1/me/calendar/events), one occurrence or the series moved,
// changed and deleted, an invitation in a calendar answered by its owner or
// by a delegate on the owner's behalf (/api/v1/me/calendar/events/{id}/reply),
// the free-busy of an account of the same domain (/api/v1/me/freebusy), the
// tasks (/api/v1/me/tasks), and the invitation a message carries with the
// answer to it (/api/v1/me/messages/{id}/invitation). The objects stored are
// the iCalendar objects CalDAV serves, so a phone that syncs sees what the
// page made and the page sees what the phone made. Imip.h carries the mail
// half and FreeBusy.h the free-busy computation CalDAV shares.

#include "StdAfx.h"

#include <ctime>
#include <climits>
#include <cstdlib>
#include <algorithm>

#include "RestApiServer.h"
#include "HttpServer.h"
#include "CalendarStore.h"
#include "ICalendar.h"
#include "Imip.h"
#include "FreeBusy.h"
#include "ResourceBooking.h"
#include "DavSupport.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "GUIDCreator.h"
#include "FileUtilities.h"
#include "../BO/Account.h"
#include "../BO/DomainAliases.h"
#include "../BO/Group.h"
#include "../BO/Groups.h"
#include "../BO/Message.h"
#include "../BO/IMAPFolder.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentMessage.h"
#include "../Application/Configuration.h"
#include "../Application/ObjectCache.h"
#include "../Application/Logger.h"
#include "../../IMAP/IMAPConfiguration.h"
#include "Parsing/StringParser.h"

namespace HM
{
   namespace
   {
      AnsiString Int64Text(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      AnsiString IntText(int value)
      {
         AnsiString text;
         text.Format("%d", value);
         return text;
      }

      AnsiString U8(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      // The same escaping RestApiServer::JsonEscape_ applies, for the helpers
      // outside the class.
      AnsiString Esc(const AnsiString &value)
      {
         AnsiString out;
         out.reserve(value.GetLength() + 8);
         for (int i = 0; i < value.GetLength(); i++)
         {
            unsigned char c = static_cast<unsigned char>(value.c_str()[i]);
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
               if (c < 0x20)
               {
                  AnsiString hex;
                  hex.Format("\\u%04x", c);
                  out += hex;
               }
               else
                  out += static_cast<char>(c);
            }
         }
         return out;
      }

      AnsiString Lower(const AnsiString &value)
      {
         AnsiString lower = value;
         lower.ToLower();
         return lower;
      }

      AnsiString Trimmed(const AnsiString &value)
      {
         AnsiString trimmed = value;
         trimmed.TrimLeft();
         trimmed.TrimRight();
         return trimmed;
      }

      __int64 NowUtc()
      {
         return static_cast<__int64>(time(nullptr));
      }

      // A calendar the account can see: its own, or one shared with it - by
      // name or through a group - read-only, read-write, or as a delegate who
      // may answer the owner's invitations in it.
      struct Visible
      {
         Visible() : mine(true), writable(true), delegate(false) { }

         CalendarRecord calendar;
         bool mine;
         bool writable;
         bool delegate;
      };

      bool VisibleCalendars(std::shared_ptr<const Account> account, std::vector<Visible> &visible)
      {
         visible.clear();

         std::vector<CalendarRecord> own;
         if (!CalendarStore::ListCalendars(account->GetID(), own))
            return false;
         for (size_t i = 0; i < own.size(); i++)
         {
            Visible v;
            v.calendar = own[i];
            visible.push_back(v);
         }

         std::vector<CalendarRecord> shared;
         std::vector<CalendarShareRecord> shares;
         if (CalendarStore::ListSharedWith(account->GetID(), shared, shares))
         {
            for (size_t i = 0; i < shared.size(); i++)
            {
               Visible v;
               v.calendar = shared[i];
               v.mine = false;
               v.writable = shares[i].write;
               v.delegate = shares[i].delegate;
               visible.push_back(v);
            }
         }
         return true;
      }

      bool FindVisible(std::shared_ptr<const Account> account, __int64 calendarId, Visible &found)
      {
         std::vector<Visible> visible;
         if (!VisibleCalendars(account, visible))
            return false;
         for (size_t i = 0; i < visible.size(); i++)
         {
            if (visible[i].calendar.id == calendarId)
            {
               found = visible[i];
               return true;
            }
         }
         return false;
      }

      // The object by its id, if it sits in a calendar the account can see.
      bool FindObject(std::shared_ptr<const Account> account, __int64 objectId, Visible &visible, CalendarObjectRecord &record)
      {
         CalendarObjectRecord candidate;
         if (!CalendarStore::FindById(objectId, candidate))
            return false;
         if (!FindVisible(account, candidate.calendarId, visible))
            return false;
         record = candidate;
         return true;
      }

      // What a share names: an account of this server by its address first,
      // else one of the server's groups by its name - so a name that is both
      // is the account. False when it is neither.
      bool ResolveShareTarget(const AnsiString &name, std::shared_ptr<const Account> &other, std::shared_ptr<Group> &group)
      {
         other.reset();
         group.reset();

         String wanted;
         Unicode::MultiByteToWide(Trimmed(name), wanted);
         if (wanted.IsEmpty() || wanted.GetLength() > 255)
            return false;

         String lower = wanted;
         lower.ToLower();
         if (StringParser::IsValidEmailAddress(lower))
         {
            other = CacheContainer::Instance()->GetAccount(lower);
            if (other)
               return true;
         }

         // The live collection, read again first, as the groups route reads
         // it: the Control Panel writes the same table over COM.
         std::shared_ptr<Groups> groups = Configuration::Instance()->GetIMAPConfiguration()->GetGroups();
         if (!groups)
            return false;
         groups->Refresh();
         group = groups->GetItemByName(wanted);
         return group != nullptr;
      }

      // The name of one of the server's groups by its id, or "" when it is gone.
      String GroupNameOf(__int64 groupId)
      {
         std::shared_ptr<Groups> groups = Configuration::Instance()->GetIMAPConfiguration()->GetGroups();
         if (!groups)
            return String();
         groups->Refresh();
         std::shared_ptr<Group> group = groups->GetItemByDBID(static_cast<unsigned __int64>(groupId));
         return group ? group->GetName() : String();
      }

      AnsiString ShareJson(const CalendarShareRecord &share)
      {
         AnsiString json = share.groupId != 0
            ? "{\"group\":\"" + Esc(U8(share.groupName)) + "\",\"members\":" + IntText(share.members)
            : "{\"address\":\"" + Esc(Lower(U8(share.address))) + "\"";
         return json + ",\"write\":" + AnsiString(share.write ? "true" : "false") +
            ",\"delegate\":" + AnsiString(share.delegate ? "true" : "false") + "}";
      }

      AnsiString CalendarJson(std::shared_ptr<const Account> account, const Visible &v, bool withShares)
      {
         AnsiString owner;
         if (v.mine)
            owner = U8(account->GetAddress());
         else
         {
            std::shared_ptr<const Account> other = CacheContainer::Instance()->GetAccount(v.calendar.accountId);
            owner = other ? U8(other->GetAddress()) : AnsiString("");
         }
         bool isDefault = v.calendar.name.CompareNoCase(String(CalendarStore::DefaultName)) == 0;

         AnsiString json = "{\"id\":" + Int64Text(v.calendar.id) +
            ",\"name\":\"" + Esc(U8(v.calendar.name)) +
            "\",\"display_name\":\"" + Esc(U8(v.calendar.displayName)) +
            "\",\"color\":\"" + Esc(U8(v.calendar.color)) +
            "\",\"owner\":\"" + Esc(Lower(owner)) +
            "\",\"mine\":" + AnsiString(v.mine ? "true" : "false") +
            ",\"writable\":" + AnsiString(v.writable ? "true" : "false") +
            ",\"delegate\":" + AnsiString(!v.mine && v.delegate ? "true" : "false") +
            ",\"is_default\":" + AnsiString(isDefault && v.mine ? "true" : "false") +
            ",\"sync_token\":" + Int64Text(v.calendar.syncToken);

         if (withShares && v.mine)
         {
            std::vector<CalendarShareRecord> shares;
            CalendarStore::ListShares(v.calendar.id, shares);
            json += ",\"shares\":[";
            for (size_t i = 0; i < shares.size(); i++)
            {
               if (i > 0)
                  json += ",";
               json += ShareJson(shares[i]);
            }
            json += "]";
         }
         return json + "}";
      }

      //---------------------------------------------------------------------
      // Times as the page writes them
      //---------------------------------------------------------------------

      bool Digits(const AnsiString &text, int from, int count, int &value)
      {
         if (from + count > text.GetLength())
            return false;
         value = 0;
         for (int i = from; i < from + count; i++)
         {
            char c = text.c_str()[i];
            if (c < '0' || c > '9')
               return false;
            value = value * 10 + (c - '0');
         }
         return true;
      }

      // "YYYY-MM-DD", "YYYY-MM-DDTHH:MM", "YYYY-MM-DDTHH:MM:SS", or the
      // iCalendar spellings "YYYYMMDD", "YYYYMMDDTHHMMSS", "YYYYMMDDTHHMMSSZ".
      bool ParseWall(const AnsiString &raw, bool &isDate, bool &utc, int &y, int &m, int &d, int &hh, int &mm, int &ss)
      {
         AnsiString text = Trimmed(raw);
         isDate = false;
         utc = false;
         hh = mm = ss = 0;
         if (text.GetLength() >= 10 && text.c_str()[4] == '-' && text.c_str()[7] == '-')
         {
            if (!Digits(text, 0, 4, y) || !Digits(text, 5, 2, m) || !Digits(text, 8, 2, d))
               return false;
            if (text.GetLength() == 10)
            {
               isDate = true;
               return m >= 1 && m <= 12 && d >= 1 && d <= 31;
            }
            if (text.c_str()[10] != 'T' && text.c_str()[10] != ' ')
               return false;
            if (!Digits(text, 11, 2, hh) || text.GetLength() < 16 || text.c_str()[13] != ':' || !Digits(text, 14, 2, mm))
               return false;
            if (text.GetLength() >= 19 && text.c_str()[16] == ':')
               Digits(text, 17, 2, ss);
            if (text.EndsWith("Z"))
               utc = true;
            return m >= 1 && m <= 12 && d >= 1 && d <= 31 && hh < 24 && mm < 60 && ss < 60;
         }
         if (text.GetLength() >= 8)
         {
            if (!Digits(text, 0, 4, y) || !Digits(text, 4, 2, m) || !Digits(text, 6, 2, d))
               return false;
            if (text.GetLength() == 8)
            {
               isDate = true;
               return m >= 1 && m <= 12 && d >= 1 && d <= 31;
            }
            if (text.c_str()[8] != 'T' || !Digits(text, 9, 2, hh) || !Digits(text, 11, 2, mm))
               return false;
            Digits(text, 13, 2, ss);
            utc = text.EndsWith("Z");
            return m >= 1 && m <= 12 && d >= 1 && d <= 31 && hh < 24 && mm < 60 && ss < 60;
         }
         return false;
      }

      AnsiString DateText(int y, int m, int d)
      {
         AnsiString text;
         text.Format("%04d%02d%02d", y, m, d);
         return text;
      }

      AnsiString DateTimeText(int y, int m, int d, int hh, int mm, int ss)
      {
         AnsiString text;
         text.Format("%04d%02d%02dT%02d%02d%02d", y, m, d, hh, mm, ss);
         return text;
      }

      // "YYYY-MM-DDTHH:MM" in a zone, for the editor; "YYYY-MM-DD" for a day.
      AnsiString LocalText(const AnsiString &tzid, __int64 instant, bool allDay)
      {
         int y, m, d, hh, mm, ss;
         if (!ICalendar::InstantToLocal(allDay || tzid.IsEmpty() ? AnsiString("UTC") : tzid, instant, y, m, d, hh, mm, ss))
            ICalendar::InstantToLocal("UTC", instant, y, m, d, hh, mm, ss);
         AnsiString text;
         if (allDay)
            text.Format("%04d-%02d-%02d", y, m, d);
         else
            text.Format("%04d-%02d-%02dT%02d:%02d", y, m, d, hh, mm);
         return text;
      }

      //---------------------------------------------------------------------
      // The component
      //---------------------------------------------------------------------

      ICalComponent *MasterOf(ICalComponent &calendar)
      {
         ICalComponent *first = nullptr;
         for (size_t i = 0; i < calendar.children.size(); i++)
         {
            ICalComponent &component = calendar.children[i];
            if (component.name != "VEVENT" && component.name != "VTODO")
               continue;
            if (!component.Property("RECURRENCE-ID"))
               return &component;
            if (!first)
               first = &component;
         }
         return first;
      }

      const ICalComponent *MasterOf(const ICalComponent &calendar)
      {
         return MasterOf(const_cast<ICalComponent &>(calendar));
      }

      void RemoveProperties(ICalComponent &component, const char *name)
      {
         for (size_t i = 0; i < component.properties.size();)
         {
            if (component.properties[i].name == name)
               component.properties.erase(component.properties.begin() + i);
            else
               i++;
         }
      }

      void SetProperty(ICalComponent &component, const ICalProperty &property)
      {
         for (size_t i = 0; i < component.properties.size(); i++)
         {
            if (component.properties[i].name == property.name)
            {
               component.properties[i] = property;
               // Any further one of the name goes, so the property is single.
               for (size_t j = i + 1; j < component.properties.size();)
               {
                  if (component.properties[j].name == property.name)
                     component.properties.erase(component.properties.begin() + j);
                  else
                     j++;
               }
               return;
            }
         }
         component.properties.push_back(property);
      }

      void SetText(ICalComponent &component, const char *name, const AnsiString &text)
      {
         if (text.IsEmpty())
         {
            RemoveProperties(component, name);
            return;
         }
         ICalProperty property;
         property.name = name;
         property.value = ICalendar::Escape(text);
         SetProperty(component, property);
      }

      void SetPlain(ICalComponent &component, const char *name, const AnsiString &value)
      {
         if (value.IsEmpty())
         {
            RemoveProperties(component, name);
            return;
         }
         ICalProperty property;
         property.name = name;
         property.value = value;
         SetProperty(component, property);
      }

      // SEQUENCE as an int, a number past an int's range held to its end on both
      // platforms: atoi gave INT_MAX on Windows and the low 32 bits on Linux,
      // where 4294967297 read as 1.
      int SequenceOf(const ICalComponent &component)
      {
         AnsiString text = Trimmed(component.Value("SEQUENCE"));
         const long long value = text.IsEmpty() ? 0 : strtoll(text.c_str(), nullptr, 10);
         return value > INT_MAX ? INT_MAX : (value < INT_MIN ? INT_MIN : static_cast<int>(value));
      }

      // The SEQUENCE an edit writes: one more, held at INT_MAX, where adding one
      // would wrap to INT_MIN and make the edit older than what it replaces.
      int NextSequence(const ICalComponent &component)
      {
         const int sequence = SequenceOf(component);
         return sequence < INT_MAX ? sequence + 1 : INT_MAX;
      }

      // PRIORITY as the task routes write it and RFC 5545 3.8.1.9 holds it: 0
      // (none) to 9. What another client wrote outside that reads as none.
      int PriorityOf(const AnsiString &text)
      {
         const long value = text.IsEmpty() ? 0 : strtol(text.c_str(), nullptr, 10);
         return value >= 0 && value <= 9 ? static_cast<int>(value) : 0;
      }

      // -PT15M, -PT1H, -P1D, PT0S -> minutes before the start; -1 when the
      // component has no display alarm. The TRIGGER is any calendar client's
      // text, and an invitation's is its sender's: a duration past an int's
      // range is held to its end, 2147483647 minutes, as SEQUENCE is. The digits
      // and the sum were ints with no bound, and overflowed.
      int AlarmMinutes(const ICalComponent &component)
      {
         for (size_t i = 0; i < component.children.size(); i++)
         {
            const ICalComponent &alarm = component.children[i];
            if (alarm.name != "VALARM")
               continue;
            AnsiString trigger = Trimmed(alarm.Value("TRIGGER"));
            trigger.ToUpper();
            if (trigger.IsEmpty())
               continue;
            bool negative = trigger.StartsWith("-");
            AnsiString rest = negative ? trigger.Mid(1) : (trigger.StartsWith("+") ? trigger.Mid(1) : trigger);
            if (!rest.StartsWith("P"))
               continue;
            long long minutes = 0;
            long long number = 0;
            bool inTime = false;
            for (int k = 1; k < rest.GetLength(); k++)
            {
               char c = rest.c_str()[k];
               if (c >= '0' && c <= '9')
               {
                  // A digit run stops growing once it is past an int, so that
                  // neither it nor a week's product of it can overflow here.
                  if (number <= INT_MAX)
                     number = number * 10 + (c - '0');
               }
               else if (c == 'T')
                  inTime = true;
               else
               {
                  if (c == 'W') minutes += number * 7 * 1440;
                  else if (c == 'D') minutes += number * 1440;
                  else if (c == 'H') minutes += number * 60;
                  else if (c == 'M' && inTime) minutes += number;
                  else if (c == 'S') minutes += number / 60;
                  if (minutes > INT_MAX)
                     minutes = INT_MAX;
                  number = 0;
               }
            }
            return negative ? static_cast<int>(minutes) : 0;
         }
         return -1;
      }

      void SetAlarm(ICalComponent &component, int minutes)
      {
         for (size_t i = 0; i < component.children.size();)
         {
            if (component.children[i].name == "VALARM")
               component.children.erase(component.children.begin() + i);
            else
               i++;
         }
         if (minutes < 0)
            return;
         ICalComponent alarm;
         alarm.name = "VALARM";
         ICalProperty action;
         action.name = "ACTION";
         action.value = "DISPLAY";
         alarm.properties.push_back(action);
         ICalProperty description;
         description.name = "DESCRIPTION";
         description.value = "Reminder";
         alarm.properties.push_back(description);
         ICalProperty trigger;
         trigger.name = "TRIGGER";
         trigger.value = minutes == 0 ? AnsiString("PT0M") : "-PT" + IntText(minutes) + "M";
         alarm.properties.push_back(trigger);
         component.children.push_back(alarm);
      }

      struct Attendee
      {
         AnsiString address;
         AnsiString name;
      };

      // "Ada Lovelace <ada@example.org>", "ada@example.org", or a comma list.
      void SplitAttendees(const AnsiString &text, std::vector<Attendee> &out)
      {
         AnsiString current;
         bool quoted = false;
         for (int i = 0; i <= text.GetLength(); i++)
         {
            char c = i < text.GetLength() ? text.c_str()[i] : ',';
            if (c == '"')
               quoted = !quoted;
            if ((c == ',' || c == ';') && !quoted)
            {
               AnsiString entry = Trimmed(current);
               current = "";
               if (entry.IsEmpty())
                  continue;
               Attendee attendee;
               int open = entry.Find("<");
               int close = entry.Find(">");
               if (open >= 0 && close > open)
               {
                  attendee.address = Lower(Trimmed(entry.Mid(open + 1, close - open - 1)));
                  attendee.name = Trimmed(entry.Mid(0, open));
                  if (attendee.name.GetLength() >= 2 && attendee.name.StartsWith("\"") && attendee.name.EndsWith("\""))
                     attendee.name = attendee.name.Mid(1, attendee.name.GetLength() - 2);
               }
               else
                  attendee.address = Lower(entry);
               if (attendee.address.Find("@") > 0)
                  out.push_back(attendee);
               continue;
            }
            current += c;
         }
      }

      void SetAttendees(ICalComponent &component, const std::vector<Attendee> &attendees, const AnsiString &organizer, const AnsiString &organizerName)
      {
         // What each attendee had answered survives the list being written again.
         std::vector<ICalProperty> previous;
         for (size_t i = 0; i < component.properties.size(); i++)
         {
            if (component.properties[i].name == "ATTENDEE")
               previous.push_back(component.properties[i]);
         }
         RemoveProperties(component, "ATTENDEE");

         for (size_t i = 0; i < attendees.size(); i++)
         {
            ICalProperty property;
            property.name = "ATTENDEE";
            AnsiString partstat = "NEEDS-ACTION";
            for (size_t p = 0; p < previous.size(); p++)
            {
               if (Imip::AddressOf(previous[p].value) == attendees[i].address)
               {
                  AnsiString had = previous[p].Parameter("PARTSTAT");
                  if (!had.IsEmpty())
                     partstat = had;
               }
            }
            if (!attendees[i].name.IsEmpty())
               property.parameters.push_back(std::make_pair(AnsiString("CN"), attendees[i].name));
            property.parameters.push_back(std::make_pair(AnsiString("ROLE"), AnsiString("REQ-PARTICIPANT")));
            property.parameters.push_back(std::make_pair(AnsiString("PARTSTAT"), partstat));
            property.parameters.push_back(std::make_pair(AnsiString("RSVP"), AnsiString("TRUE")));
            property.value = "mailto:" + attendees[i].address;
            component.properties.push_back(property);
         }

         if (attendees.empty())
            RemoveProperties(component, "ORGANIZER");
         else
         {
            ICalProperty property;
            property.name = "ORGANIZER";
            if (!organizerName.IsEmpty())
               property.parameters.push_back(std::make_pair(AnsiString("CN"), organizerName));
            property.value = "mailto:" + organizer;
            SetProperty(component, property);
         }
      }

      // What the page sends for an event or a task. Every field optional on an
      // update; the create needs a summary and a start.
      struct Fields
      {
         Fields() : hasSummary(false), hasLocation(false), hasDescription(false), hasAllDay(false), allDay(false),
                    hasStart(false), hasEnd(false), hasTz(false), hasRrule(false), hasAlarm(false), alarmMinutes(-1),
                    hasAttendees(false), hasPriority(false), priority(0), hasCompleted(false), completed(false), hasDue(false), hasStatus(false) { }

         bool hasSummary; AnsiString summary;
         bool hasLocation; AnsiString location;
         bool hasDescription; AnsiString description;
         bool hasAllDay; bool allDay;
         bool hasStart; AnsiString start;
         bool hasEnd; AnsiString end;
         bool hasTz; AnsiString tz;
         bool hasRrule; AnsiString rrule;
         bool hasAlarm; int alarmMinutes;
         bool hasAttendees; std::vector<Attendee> attendees;
         bool hasPriority; int priority;
         bool hasCompleted; bool completed;
         bool hasDue; AnsiString due;
         bool hasStatus; AnsiString status;
      };

      AnsiString StringOf(const JsonValue *value)
      {
         return value && value->IsString() ? AnsiString(value->AsString().c_str()) : AnsiString("");
      }

      bool ReadFields(const JsonValue &document, Fields &fields, AnsiString &problem)
      {
         if (const JsonValue *v = document.Get("summary")) { fields.hasSummary = true; fields.summary = Trimmed(StringOf(v)); }
         if (const JsonValue *v = document.Get("location")) { fields.hasLocation = true; fields.location = Trimmed(StringOf(v)); }
         if (const JsonValue *v = document.Get("description")) { fields.hasDescription = true; fields.description = StringOf(v); }
         if (const JsonValue *v = document.Get("all_day")) { fields.hasAllDay = true; fields.allDay = v->AsBool(); }
         if (const JsonValue *v = document.Get("start")) { fields.hasStart = true; fields.start = Trimmed(StringOf(v)); }
         if (const JsonValue *v = document.Get("end")) { fields.hasEnd = true; fields.end = Trimmed(StringOf(v)); }
         if (const JsonValue *v = document.Get("due")) { fields.hasDue = true; fields.due = Trimmed(StringOf(v)); }
         if (const JsonValue *v = document.Get("tz")) { fields.hasTz = true; fields.tz = Trimmed(StringOf(v)); }
         if (const JsonValue *v = document.Get("rrule")) { fields.hasRrule = true; fields.rrule = Trimmed(StringOf(v)); fields.rrule.ToUpper(); }
         if (const JsonValue *v = document.Get("status")) { fields.hasStatus = true; fields.status = Trimmed(StringOf(v)); fields.status.ToUpper(); }
         if (const JsonValue *v = document.Get("alarm_minutes"))
         {
            fields.hasAlarm = true;
            // Held to its range as the number it is, before it is an int: a number
            // past an int's range made the conversion undefined (INT_MIN on x64),
            // which passed the check. Made an int, it is truncated as it was; -1
            // and below is no alarm.
            const double minutes = v->IsNumber() ? v->AsNumber() : -1;
            if (!(minutes < 60 * 24 * 366 + 1))
            {
               problem = "alarm_minutes is at most a year";
               return false;
            }
            fields.alarmMinutes = minutes > -1 ? static_cast<int>(minutes) : -1;
         }
         if (const JsonValue *v = document.Get("priority"))
         {
            fields.hasPriority = true;
            // The same: checked as the number it is, then truncated as it was,
            // so 2.5 is 2 and 1e20 is refused rather than undefined.
            const double priority = v->IsNumber() ? v->AsNumber() : 0;
            if (!(priority > -1 && priority < 10))
            {
               problem = "priority is 0 (none) to 9, 1 the highest";
               return false;
            }
            fields.priority = static_cast<int>(priority);
         }
         if (const JsonValue *v = document.Get("completed")) { fields.hasCompleted = true; fields.completed = v->AsBool(); }
         if (const JsonValue *v = document.Get("attendees"))
         {
            fields.hasAttendees = true;
            if (v->IsArray())
            {
               for (size_t i = 0; i < v->Size(); i++)
                  SplitAttendees(StringOf(v->At(i)), fields.attendees);
            }
            else
               SplitAttendees(StringOf(v), fields.attendees);
            if (fields.attendees.size() > 200)
            {
               problem = "at most 200 attendees";
               return false;
            }
         }
         if (fields.hasSummary && fields.summary.GetLength() > 1000)
         {
            problem = "summary is at most 1000 characters";
            return false;
         }
         if (fields.hasTz && !fields.tz.IsEmpty() && !ICalendar::KnowsTimeZone(fields.tz))
         {
            problem = "tz is not a zone this server knows: " + fields.tz;
            return false;
         }
         if (fields.hasRrule && !fields.rrule.IsEmpty() && !fields.rrule.StartsWith("FREQ="))
         {
            problem = "rrule must be an RFC 5545 recurrence rule beginning FREQ=";
            return false;
         }
         return true;
      }

      // A DTSTART/DTEND/DUE property from the page's wall time.
      bool TimeProperty(const char *name, const AnsiString &wall, bool allDay, const AnsiString &tz, ICalProperty &property, AnsiString &problem)
      {
         bool isDate, utc;
         int y, m, d, hh, mm, ss;
         if (!ParseWall(wall, isDate, utc, y, m, d, hh, mm, ss))
         {
            problem = AnsiString(name) + " must be YYYY-MM-DD or YYYY-MM-DDTHH:MM, or the iCalendar form";
            return false;
         }
         property = ICalProperty();
         property.name = name;
         if (allDay || isDate)
         {
            property.parameters.push_back(std::make_pair(AnsiString("VALUE"), AnsiString("DATE")));
            property.value = DateText(y, m, d);
            return true;
         }
         if (utc || tz.IsEmpty() || Lower(tz) == "utc")
         {
            property.value = DateTimeText(y, m, d, hh, mm, ss) + "Z";
            return true;
         }
         property.parameters.push_back(std::make_pair(AnsiString("TZID"), tz));
         property.value = DateTimeText(y, m, d, hh, mm, ss);
         return true;
      }

      // The change applied to one component - the master, or an override.
      bool ApplyFields(ICalComponent &component, const Fields &fields, const AnsiString &tzNow, bool allDayNow, const AnsiString &organizer, const AnsiString &organizerName, AnsiString &problem)
      {
         AnsiString tz = fields.hasTz ? fields.tz : tzNow;
         bool allDay = fields.hasAllDay ? fields.allDay : allDayNow;
         bool isTodo = component.name == "VTODO";

         if (fields.hasSummary)
            SetText(component, "SUMMARY", fields.summary);
         if (fields.hasLocation)
            SetText(component, "LOCATION", fields.location);
         if (fields.hasDescription)
            SetText(component, "DESCRIPTION", fields.description);

         if (fields.hasStart || fields.hasAllDay || fields.hasTz)
         {
            AnsiString wall = fields.hasStart ? fields.start : AnsiString("");
            if (wall.IsEmpty())
            {
               // The kind changed and not the time: the start stays where it
               // was, written in the new form.
               const ICalProperty *had = component.Property("DTSTART");
               if (had)
                  wall = had->value;
            }
            if (!wall.IsEmpty())
            {
               ICalProperty property;
               if (!TimeProperty("DTSTART", wall, allDay, tz, property, problem))
                  return false;
               SetProperty(component, property);
            }
            else if (isTodo)
               RemoveProperties(component, "DTSTART");
         }

         if (fields.hasEnd || fields.hasAllDay || fields.hasTz)
         {
            AnsiString wall = fields.hasEnd ? fields.end : AnsiString("");
            if (wall.IsEmpty())
            {
               const ICalProperty *had = component.Property(isTodo ? "DUE" : "DTEND");
               if (had && !fields.hasEnd)
                  wall = had->value;
            }
            RemoveProperties(component, "DURATION");
            if (!wall.IsEmpty())
            {
               ICalProperty property;
               if (!TimeProperty(isTodo ? "DUE" : "DTEND", wall, allDay, tz, property, problem))
                  return false;
               SetProperty(component, property);
            }
            else
               RemoveProperties(component, isTodo ? "DUE" : "DTEND");
         }

         if (fields.hasDue)
         {
            RemoveProperties(component, "DUE");
            if (!fields.due.IsEmpty())
            {
               ICalProperty property;
               if (!TimeProperty("DUE", fields.due, allDay, tz, property, problem))
                  return false;
               SetProperty(component, property);
            }
         }

         if (fields.hasRrule)
            SetPlain(component, "RRULE", fields.rrule);
         if (fields.hasAlarm)
            SetAlarm(component, fields.alarmMinutes);
         if (fields.hasAttendees)
            SetAttendees(component, fields.attendees, organizer, organizerName);
         if (fields.hasPriority)
            SetPlain(component, "PRIORITY", fields.priority == 0 ? AnsiString("") : IntText(fields.priority));
         if (fields.hasStatus)
            SetPlain(component, "STATUS", fields.status);
         if (fields.hasCompleted)
         {
            if (fields.completed)
            {
               SetPlain(component, "STATUS", "COMPLETED");
               SetPlain(component, "COMPLETED", ICalendar::FormatUtc(NowUtc()));
               SetPlain(component, "PERCENT-COMPLETE", "100");
            }
            else
            {
               SetPlain(component, "STATUS", isTodo ? AnsiString("NEEDS-ACTION") : AnsiString(""));
               RemoveProperties(component, "COMPLETED");
               RemoveProperties(component, "PERCENT-COMPLETE");
            }
         }

         SetPlain(component, "DTSTAMP", ICalendar::FormatUtc(NowUtc()));
         SetPlain(component, "LAST-MODIFIED", ICalendar::FormatUtc(NowUtc()));
         return true;
      }

      AnsiString AttendeesText(const ICalComponent &component)
      {
         AnsiString text;
         for (size_t i = 0; i < component.properties.size(); i++)
         {
            if (component.properties[i].name == "ATTENDEE")
               text += component.properties[i].value + "\n";
         }
         return text;
      }

      // What a change to the series carries into one of its overrides: each
      // field the change sets that the occurrence still had as the series had
      // it before. A weekly meeting renamed renames the week that was moved to
      // the afternoon, and that week stays in the afternoon; a week renamed on
      // its own keeps its name. Times are never carried - they are what an
      // override is usually for.
      Fields FieldsToFollow(const Fields &fields, const ICalComponent &before, const ICalComponent &occurrence)
      {
         Fields follow;
         if (fields.hasSummary && occurrence.Value("SUMMARY") == before.Value("SUMMARY")) { follow.hasSummary = true; follow.summary = fields.summary; }
         if (fields.hasLocation && occurrence.Value("LOCATION") == before.Value("LOCATION")) { follow.hasLocation = true; follow.location = fields.location; }
         if (fields.hasDescription && occurrence.Value("DESCRIPTION") == before.Value("DESCRIPTION")) { follow.hasDescription = true; follow.description = fields.description; }
         if (fields.hasAlarm && AlarmMinutes(occurrence) == AlarmMinutes(before)) { follow.hasAlarm = true; follow.alarmMinutes = fields.alarmMinutes; }
         if (fields.hasPriority && occurrence.Value("PRIORITY") == before.Value("PRIORITY")) { follow.hasPriority = true; follow.priority = fields.priority; }
         if (fields.hasStatus && occurrence.Value("STATUS") == before.Value("STATUS")) { follow.hasStatus = true; follow.status = fields.status; }
         if (fields.hasAttendees && AttendeesText(occurrence) == AttendeesText(before)) { follow.hasAttendees = true; follow.attendees = fields.attendees; }
         return follow;
      }

      ICalComponent NewCalendar()
      {
         ICalComponent calendar;
         calendar.name = "VCALENDAR";
         ICalProperty version;
         version.name = "VERSION";
         version.value = "2.0";
         calendar.properties.push_back(version);
         ICalProperty prodid;
         prodid.name = "PRODID";
         prodid.value = "-//hMailServer//webmail//EN";
         calendar.properties.push_back(prodid);
         return calendar;
      }

      AnsiString NewUid(std::shared_ptr<const Account> account)
      {
         AnsiString me = Lower(U8(account->GetAddress()));
         AnsiString domain = me.Find("@") >= 0 ? me.Mid(me.Find("@") + 1) : me;
         return Lower(U8(GUIDCreator::GetGUID())) + "@" + domain;
      }

      AnsiString OrganizerName(std::shared_ptr<const Account> account)
      {
         String name = account->GetPersonFirstName();
         if (!account->GetPersonLastName().IsEmpty())
            name += (name.IsEmpty() ? _T("") : _T(" ")) + account->GetPersonLastName();
         return Trimmed(U8(name));
      }

      // The object written to the store: parsed again, summarised, stamped. A
      // room's or a piece of equipment's calendar is written under the lock
      // its bookings are decided under (ResourceCalendarGuard), so that a
      // delegate's change never lands in the middle of a decision.
      bool Store(const Visible &visible, const ICalComponent &tree, CalendarObjectRecord &record, bool insert, AnsiString &problem)
      {
         ResourceCalendarGuard guard(visible.calendar.accountId);
         AnsiString serialized = ICalendar::Serialize(tree);
         ICalComponent parsed;
         if (!ICalendar::Parse(serialized, parsed, problem))
            return false;
         ICalendar::Summary summary;
         if (!ICalendar::Summarize(parsed, summary, problem))
            return false;

         String data;
         Unicode::MultiByteToWide(serialized, data);
         record.data = data;
         record.etag = Dav::Wide(Dav::EtagOf(serialized));
         String uid;
         Unicode::MultiByteToWide(summary.uid, uid);
         record.uid = uid;
         String component;
         Unicode::MultiByteToWide(summary.component, component);
         record.component = component;
         record.start = summary.start;
         record.end = summary.end;
         record.first = summary.first;
         record.last = summary.last;

         __int64 token = 0;
         if (insert)
         {
            if (record.uri.IsEmpty())
               record.uri = Imip::UriFor(summary.uid);
            CalendarObjectRecord inserted;
            if (!CalendarStore::Insert(visible.calendar.accountId, visible.calendar.id, record, inserted, token))
            {
               problem = "the object could not be stored";
               return false;
            }
            record = inserted;
            return true;
         }
         if (!CalendarStore::Update(visible.calendar.accountId, visible.calendar.id, record.id, record, token))
         {
            problem = "the object could not be saved";
            return false;
         }
         return true;
      }

      AnsiString MyPartstat(const ICalComponent &component, const AnsiString &me)
      {
         for (size_t i = 0; i < component.properties.size(); i++)
         {
            const ICalProperty &property = component.properties[i];
            if (property.name == "ATTENDEE" && Imip::AddressOf(property.value) == me)
            {
               AnsiString partstat = property.Parameter("PARTSTAT");
               partstat.ToUpper();
               return partstat.IsEmpty() ? AnsiString("NEEDS-ACTION") : partstat;
            }
         }
         return "";
      }

      AnsiString AttendeesJson(const ICalComponent &component)
      {
         AnsiString json = "[";
         bool first = true;
         for (size_t i = 0; i < component.properties.size(); i++)
         {
            const ICalProperty &property = component.properties[i];
            if (property.name != "ATTENDEE")
               continue;
            AnsiString partstat = property.Parameter("PARTSTAT");
            partstat.ToUpper();
            if (!first)
               json += ",";
            first = false;
            json += "{\"address\":\"" + Esc(Imip::AddressOf(property.value)) +
               "\",\"name\":\"" + Esc(property.Parameter("CN")) +
               "\",\"partstat\":\"" + Esc(partstat.IsEmpty() ? AnsiString("NEEDS-ACTION") : partstat) +
               "\",\"sent_by\":\"" + Esc(Imip::AddressOf(property.Parameter("SENT-BY"))) + "\"}";
         }
         return json + "]";
      }

      // The fields of one component the page shows and edits.
      AnsiString ComponentFieldsJson(const ICalComponent &component, const ICalComponent &tree, const AnsiString &me)
      {
         AnsiString tz;
         bool allDay = false;
         if (const ICalProperty *dtstart = component.Property("DTSTART"))
         {
            tz = dtstart->Parameter("TZID");
            allDay = dtstart->HasParameter("VALUE", "DATE");
            if (tz.IsEmpty() && !allDay && !dtstart->value.EndsWith("Z"))
               tz = "";
         }
         AnsiString organizer = Imip::AddressOf(component.Value("ORGANIZER"));
         AnsiString organizerName;
         if (const ICalProperty *property = component.Property("ORGANIZER"))
            organizerName = property->Parameter("CN");
         AnsiString status = Trimmed(component.Value("STATUS"));
         status.ToUpper();

         return "\"summary\":\"" + Esc(ICalendar::Unescape(component.Value("SUMMARY"))) +
            "\",\"location\":\"" + Esc(ICalendar::Unescape(component.Value("LOCATION"))) +
            "\",\"description\":\"" + Esc(ICalendar::Unescape(component.Value("DESCRIPTION"))) +
            "\",\"tz\":\"" + Esc(tz) +
            "\",\"rrule\":\"" + Esc(component.Value("RRULE")) +
            "\",\"alarm_minutes\":" + IntText(AlarmMinutes(component)) +
            ",\"organizer\":\"" + Esc(organizer) +
            "\",\"organizer_name\":\"" + Esc(organizerName) +
            "\",\"attendees\":" + AttendeesJson(component) +
            ",\"my_partstat\":\"" + Esc(MyPartstat(component, me)) +
            "\",\"status\":\"" + Esc(status) +
            "\",\"sequence\":" + IntText(SequenceOf(component)) +
            ",\"all_day\":" + AnsiString(allDay ? "true" : "false");
      }

      AnsiString InstanceJson(const Visible &visible, const CalendarObjectRecord &record, const ICalComponent &tree, const ICalInstance &instance, const AnsiString &me)
      {
         const ICalComponent *component = instance.component < tree.children.size() ? &tree.children[instance.component] : MasterOf(tree);
         const ICalComponent *master = MasterOf(tree);
         if (!component)
            return "";
         AnsiString tz;
         if (const ICalProperty *dtstart = component->Property("DTSTART"))
            tz = dtstart->Parameter("TZID");
         bool recurring = master && (master->Property("RRULE") || master->Property("RDATE"));

         return "{\"id\":" + Int64Text(record.id) +
            ",\"calendar\":" + Int64Text(visible.calendar.id) +
            ",\"uid\":\"" + Esc(U8(record.uid)) +
            "\",\"start\":\"" + ICalendar::FormatUtc(instance.start) +
            "\",\"end\":\"" + ICalendar::FormatUtc(instance.end) +
            "\",\"local_start\":\"" + LocalText(tz, instance.start, instance.allDay) +
            "\",\"local_end\":\"" + LocalText(tz, instance.end, instance.allDay) +
            "\",\"recurring\":" + AnsiString(recurring ? "true" : "false") +
            ",\"recurrence_id\":\"" + (recurring ? ICalendar::FormatUtc(instance.recurrenceId) : AnsiString("")) +
            "\",\"overridden\":" + AnsiString(instance.overridden ? "true" : "false") +
            ",\"writable\":" + AnsiString(visible.writable ? "true" : "false") +
            ",\"mine\":" + AnsiString(visible.mine ? "true" : "false") +
            ",\"delegate\":" + AnsiString(!visible.mine && visible.delegate ? "true" : "false") +
            ",\"color\":\"" + Esc(U8(visible.calendar.color)) + "\"," +
            ComponentFieldsJson(*component, tree, me) + "}";
      }

      AnsiString TaskJson(const Visible &visible, const CalendarObjectRecord &record, const ICalComponent &tree, const AnsiString &me)
      {
         const ICalComponent *component = MasterOf(tree);
         if (!component)
            return "";
         AnsiString due;
         AnsiString dueLocal;
         bool allDay = false;
         AnsiString tz;
         if (const ICalProperty *property = component->Property("DUE"))
         {
            ICalTime time;
            AnsiString ignored;
            if (ICalendar::ParseTime(*property, &tree, time, ignored))
            {
               due = ICalendar::FormatUtc(time.instant);
               allDay = time.isDate;
               tz = time.tzid;
               dueLocal = LocalText(tz, time.instant, allDay);
            }
         }
         AnsiString status = Trimmed(component->Value("STATUS"));
         status.ToUpper();
         bool completed = status == "COMPLETED" || !component->Value("COMPLETED").IsEmpty();
         AnsiString priority = Trimmed(component->Value("PRIORITY"));

         return "{\"id\":" + Int64Text(record.id) +
            ",\"calendar\":" + Int64Text(visible.calendar.id) +
            ",\"uid\":\"" + Esc(U8(record.uid)) +
            "\",\"summary\":\"" + Esc(ICalendar::Unescape(component->Value("SUMMARY"))) +
            "\",\"description\":\"" + Esc(ICalendar::Unescape(component->Value("DESCRIPTION"))) +
            "\",\"due\":\"" + due +
            "\",\"local_due\":\"" + dueLocal +
            "\",\"all_day\":" + AnsiString(allDay ? "true" : "false") +
            ",\"tz\":\"" + Esc(tz) +
            "\",\"completed\":" + AnsiString(completed ? "true" : "false") +
            ",\"completed_at\":\"" + Esc(component->Value("COMPLETED")) +
            "\",\"priority\":" + IntText(PriorityOf(priority)) +
            ",\"status\":\"" + Esc(status) +
            "\",\"writable\":" + AnsiString(visible.writable ? "true" : "false") +
            ",\"mine\":" + AnsiString(visible.mine ? "true" : "false") +
            ",\"color\":\"" + Esc(U8(visible.calendar.color)) +
            "\",\"sequence\":" + IntText(SequenceOf(*component)) + "}";
      }

      bool ParseBody(const AnsiString &requestBody, JsonValue &document, AnsiString &problem)
      {
         std::string error;
         if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.GetLength()), document, error) || !document.IsObject())
         {
            problem = "the body must be a JSON object";
            return false;
         }
         return true;
      }

      AnsiString QueryValue(const AnsiString &query, const char *key)
      {
         AnsiString needle = AnsiString(key) + "=";
         int at = 0;
         while (at <= query.GetLength())
         {
            int amp = query.Find("&", at);
            AnsiString part = amp >= 0 ? query.Mid(at, amp - at) : query.Mid(at);
            if (part.StartsWith(needle))
               return Dav::PercentDecode(part.Mid(needle.GetLength()));
            if (amp < 0)
               break;
            at = amp + 1;
         }
         return "";
      }

      // The override of the master for one recurrence, made from the master
      // when there is none yet.
      ICalComponent &OverrideFor(ICalComponent &tree, __int64 instant, bool allDay, const AnsiString &tz)
      {
         for (size_t i = 0; i < tree.children.size(); i++)
         {
            ICalComponent &component = tree.children[i];
            if (component.name != "VEVENT" && component.name != "VTODO")
               continue;
            const ICalProperty *rid = component.Property("RECURRENCE-ID");
            if (!rid)
               continue;
            ICalTime time;
            AnsiString ignored;
            if (ICalendar::ParseTime(*rid, &tree, time, ignored) && time.instant == instant)
               return component;
         }

         ICalComponent copy = *MasterOf(tree);
         RemoveProperties(copy, "RRULE");
         RemoveProperties(copy, "RDATE");
         RemoveProperties(copy, "EXDATE");
         RemoveProperties(copy, "EXRULE");

         ICalProperty rid;
         rid.name = "RECURRENCE-ID";
         if (allDay)
         {
            int y, m, d, hh, mm, ss;
            ICalendar::InstantToLocal("UTC", instant, y, m, d, hh, mm, ss);
            rid.parameters.push_back(std::make_pair(AnsiString("VALUE"), AnsiString("DATE")));
            rid.value = DateText(y, m, d);
         }
         else
            rid.value = ICalendar::FormatUtc(instant);
         SetProperty(copy, rid);

         // The occurrence's own start and end as the recurrence gave them, so
         // an edit that changes only the summary keeps the time.
         (void) tz;
         tree.children.push_back(copy);
         return tree.children.back();
      }

      // The instance's start and end written into an override that was just
      // made from the master, before the fields change it.
      void GiveInstanceTimes(ICalComponent &component, const ICalInstance &instance, const AnsiString &tz, bool allDay)
      {
         ICalProperty start;
         AnsiString problem;
         AnsiString startWall = allDay ? LocalText("UTC", instance.start, true) : LocalText(tz, instance.start, false);
         AnsiString endWall = allDay ? LocalText("UTC", instance.end, true) : LocalText(tz, instance.end, false);
         if (TimeProperty("DTSTART", startWall, allDay, tz, start, problem))
            SetProperty(component, start);
         ICalProperty end;
         if (TimeProperty(component.name == "VTODO" ? "DUE" : "DTEND", endWall, allDay, tz, end, problem))
            SetProperty(component, end);
         RemoveProperties(component, "DURATION");
      }

      bool FindInstance(const ICalComponent &tree, __int64 instant, ICalInstance &instance)
      {
         std::vector<ICalInstance> instances;
         bool truncated = false;
         AnsiString problem;
         if (!ICalendar::Expand(tree, instant - 1, instant + 1, 50, instances, truncated, problem))
            return false;
         for (size_t i = 0; i < instances.size(); i++)
         {
            if (instances[i].recurrenceId == instant || instances[i].start == instant)
            {
               instance = instances[i];
               return true;
            }
         }
         return false;
      }

      void AddExdate(ICalComponent &master, __int64 instant, bool allDay, const AnsiString &tz)
      {
         ICalProperty exdate;
         exdate.name = "EXDATE";
         if (allDay)
         {
            int y, m, d, hh, mm, ss;
            ICalendar::InstantToLocal("UTC", instant, y, m, d, hh, mm, ss);
            exdate.parameters.push_back(std::make_pair(AnsiString("VALUE"), AnsiString("DATE")));
            exdate.value = DateText(y, m, d);
         }
         else if (!tz.IsEmpty() && Lower(tz) != "utc")
         {
            int y, m, d, hh, mm, ss;
            ICalendar::InstantToLocal(tz, instant, y, m, d, hh, mm, ss);
            exdate.parameters.push_back(std::make_pair(AnsiString("TZID"), tz));
            exdate.value = DateTimeText(y, m, d, hh, mm, ss);
         }
         else
            exdate.value = ICalendar::FormatUtc(instant);
         master.properties.push_back(exdate);
      }

      void RemoveOverride(ICalComponent &tree, __int64 instant)
      {
         for (size_t i = 0; i < tree.children.size();)
         {
            ICalComponent &component = tree.children[i];
            const ICalProperty *rid = (component.name == "VEVENT" || component.name == "VTODO") ? component.Property("RECURRENCE-ID") : nullptr;
            ICalTime time;
            AnsiString ignored;
            if (rid && ICalendar::ParseTime(*rid, &tree, time, ignored) && time.instant == instant)
               tree.children.erase(tree.children.begin() + i);
            else
               i++;
         }
      }

      AnsiString TzOf(const ICalComponent &component)
      {
         if (const ICalProperty *dtstart = component.Property("DTSTART"))
            return dtstart->Parameter("TZID");
         return "";
      }

      bool AllDayOf(const ICalComponent &component)
      {
         const ICalProperty *dtstart = component.Property("DTSTART");
         return dtstart && dtstart->HasParameter("VALUE", "DATE");
      }
   }

   //------------------------------------------------------------------------
   // The calendars
   //------------------------------------------------------------------------

   HttpResponse
   RestApiServer::HandleMeCalendars_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      std::vector<Visible> visible;
      if (!VisibleCalendars(account, visible))
         return BuildResponse_(500, "{\"error\":\"the calendars could not be read\"}");

      AnsiString json = "{\"calendars\":[";
      for (size_t i = 0; i < visible.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += CalendarJson(account, visible[i], true);
      }
      json += "]}";
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeCalendarCreate_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      String displayName = JsonUtf8Value_(requestBody, "display_name");
      displayName.TrimLeft();
      displayName.TrimRight();
      if (displayName.IsEmpty())
         displayName = JsonUtf8Value_(requestBody, "name");
      String name = JsonUtf8Value_(requestBody, "name");
      name.TrimLeft();
      name.TrimRight();
      if (name.IsEmpty())
      {
         // The URI segment from the display name: letters, digits and dashes.
         AnsiString utf8 = Lower(Utf8_(displayName));
         AnsiString segment;
         for (int i = 0; i < utf8.GetLength() && segment.GetLength() < 60; i++)
         {
            char c = utf8.c_str()[i];
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
               segment += c;
            else if (!segment.IsEmpty() && !segment.EndsWith("-"))
               segment += '-';
         }
         while (segment.EndsWith("-"))
            segment = segment.Mid(0, segment.GetLength() - 1);
         if (segment.IsEmpty())
            segment = "calendar-" + Lower(Utf8_(GUIDCreator::GetGUID())).Mid(0, 8);
         Unicode::MultiByteToWide(segment, name);
      }
      if (displayName.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"display_name is required\"}");
      if (displayName.GetLength() > 255)
         return BuildResponse_(400, "{\"error\":\"display_name is at most 255 characters\"}");
      if (!CalendarStore::IsUsableCalendarName(name))
         return BuildResponse_(400, "{\"error\":\"name must be one path segment of printable characters, not beginning shared-\"}");

      String color = JsonUtf8Value_(requestBody, "color");
      color.TrimLeft();
      color.TrimRight();
      if (!color.IsEmpty() && (color.GetLength() != 7 || color.c_str()[0] != '#'))
         return BuildResponse_(400, "{\"error\":\"color must be #RRGGBB\"}");

      CalendarRecord taken;
      if (CalendarStore::FindByName(account->GetID(), name, taken))
         return BuildResponse_(409, "{\"error\":\"a calendar named that exists\",\"id\":" + Int64Text(taken.id) + "}");

      CalendarRecord created;
      if (!CalendarStore::CreateCalendar(account->GetID(), name, displayName, color, created))
         return BuildResponse_(500, "{\"error\":\"the calendar could not be made\"}");

      Visible v;
      v.calendar = created;
      return BuildResponse_(201, CalendarJson(account, v, true));
   }

   HttpResponse
   RestApiServer::HandleMeCalendarUpdate_(const Caller &caller, __int64 calendarId, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      Visible v;
      if (!FindVisible(account, calendarId, v))
         return BuildResponse_(404, "{\"error\":\"calendar not found\"}");
      if (!v.mine)
         return BuildResponse_(403, "{\"error\":\"the calendar is another account's; only its owner names or colours it\"}");

      String displayName = v.calendar.displayName;
      if (requestBody.Find("\"display_name\"") >= 0)
      {
         displayName = JsonUtf8Value_(requestBody, "display_name");
         displayName.TrimLeft();
         displayName.TrimRight();
         if (displayName.IsEmpty() || displayName.GetLength() > 255)
            return BuildResponse_(400, "{\"error\":\"display_name is 1 to 255 characters\"}");
      }
      String color = v.calendar.color;
      if (requestBody.Find("\"color\"") >= 0)
      {
         color = JsonUtf8Value_(requestBody, "color");
         color.TrimLeft();
         color.TrimRight();
         if (!color.IsEmpty() && (color.GetLength() != 7 || color.c_str()[0] != '#'))
            return BuildResponse_(400, "{\"error\":\"color must be #RRGGBB\"}");
      }

      if (!CalendarStore::UpdateCalendar(account->GetID(), calendarId, displayName, color))
         return BuildResponse_(500, "{\"error\":\"the calendar could not be saved\"}");

      Visible after;
      FindVisible(account, calendarId, after);
      return BuildResponse_(200, CalendarJson(account, after, true));
   }

   HttpResponse
   RestApiServer::HandleMeCalendarDelete_(const Caller &caller, __int64 calendarId)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      Visible v;
      if (!FindVisible(account, calendarId, v))
         return BuildResponse_(404, "{\"error\":\"calendar not found\"}");

      if (!v.mine)
      {
         // Leaving a calendar someone shared: the share goes, the calendar stays
         // theirs. A share through a group is the group's, not any member's:
         // the member leaves the group, or the owner takes the share back.
         CalendarShareRecord direct;
         if (!CalendarStore::DirectShareOf(calendarId, account->GetID(), direct))
            return BuildResponse_(403, "{\"error\":\"the calendar is shared with a group you are a member of; the share is the group's, and only its owner takes it back\"}");
         if (!CalendarStore::Unshare(calendarId, account->GetID()))
            return BuildResponse_(500, "{\"error\":\"the share could not be removed\"}");
         // The account's own row is gone; a group's row may still reach it, and
         // then the calendar has not been left - the listing shows it still.
         CalendarShareRecord still;
         if (CalendarStore::ShareOf(calendarId, account->GetID(), still) && still.groupId != 0)
            return BuildResponse_(403, "{\"left\":false,\"group\":\"" + JsonEscape_(Utf8_(GroupNameOf(still.groupId))) + "\",\"error\":\"your own share is given back, but the calendar is still shared with a group you are a member of; the group's share is the group's, and only its owner takes it back\"}");
         return BuildResponse_(200, "{\"left\":true}");
      }
      if (v.calendar.name.CompareNoCase(String(CalendarStore::DefaultName)) == 0)
         return BuildResponse_(403, "{\"error\":\"the default calendar cannot be deleted\"}");
      if (!CalendarStore::DeleteCalendar(account->GetID(), calendarId))
         return BuildResponse_(500, "{\"error\":\"the calendar could not be deleted\"}");
      // A room's records of the bookings that went with it (an account signs
      // in to delete one only while it is a person).
      ResourceBooking::ForgetGoneBookings(account->GetID());
      return BuildResponse_(200, "{\"deleted\":true}");
   }

   HttpResponse
   RestApiServer::HandleMeCalendarSharePut_(const Caller &caller, __int64 calendarId, const AnsiString &address, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      Visible v;
      if (!FindVisible(account, calendarId, v))
         return BuildResponse_(404, "{\"error\":\"calendar not found\"}");
      if (!v.mine)
         return BuildResponse_(403, "{\"error\":\"only the calendar's owner shares it\"}");

      std::shared_ptr<const Account> other;
      std::shared_ptr<Group> group;
      if (!ResolveShareTarget(address, other, group))
         return BuildResponse_(404, "{\"error\":\"no account on this server has that address, and no group of this server has that name\"}");
      if (other && other->GetID() == account->GetID())
         return BuildResponse_(400, "{\"error\":\"a calendar is already its owner's\"}");

      bool write = GetJsonBoolValue_(requestBody, "write", false);
      bool delegate = GetJsonBoolValue_(requestBody, "delegate", false);
      if (delegate)
         write = true;
      bool saved = other
         ? CalendarStore::Share(calendarId, other->GetID(), 0, write, delegate)
         : CalendarStore::Share(calendarId, 0, group->GetID(), write, delegate);
      if (!saved)
         return BuildResponse_(500, "{\"error\":\"the share could not be saved\"}");

      String with = other ? other->GetAddress() : _T("the group ") + group->GetName();
      LOG_DEBUG("Calendar: " + account->GetAddress() + " shared " + v.calendar.name + " with " + with + (delegate ? _T(" (as a delegate).") : (write ? _T(" (read and write).") : _T(" (read-only)."))));
      Visible after;
      FindVisible(account, calendarId, after);
      return BuildResponse_(200, CalendarJson(account, after, true));
   }

   HttpResponse
   RestApiServer::HandleMeCalendarShareDelete_(const Caller &caller, __int64 calendarId, const AnsiString &address)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      Visible v;
      if (!FindVisible(account, calendarId, v))
         return BuildResponse_(404, "{\"error\":\"calendar not found\"}");
      if (!v.mine)
         return BuildResponse_(403, "{\"error\":\"only the calendar's owner shares it\"}");

      // One 404 whether the name is unknown or merely not shared with: the
      // groups are an administrator's list, and a DELETE by name must not read
      // it for anybody with a calendar. (The PUT's answer, which makes the
      // share and says the group's size, is the feature.)
      const char *notShared = "{\"error\":\"the calendar is not shared with that name\"}";
      std::shared_ptr<const Account> other;
      std::shared_ptr<Group> group;
      if (!ResolveShareTarget(address, other, group))
         return BuildResponse_(404, notShared);

      CalendarShareRecord share;
      if (other)
      {
         if (!CalendarStore::DirectShareOf(calendarId, other->GetID(), share))
            return BuildResponse_(404, notShared);
         if (!CalendarStore::Unshare(calendarId, other->GetID()))
            return BuildResponse_(500, "{\"error\":\"the share could not be removed\"}");
      }
      else
      {
         if (!CalendarStore::GroupShareOf(calendarId, group->GetID(), share))
            return BuildResponse_(404, notShared);
         if (!CalendarStore::UnshareGroup(calendarId, group->GetID()))
            return BuildResponse_(500, "{\"error\":\"the share could not be removed\"}");
      }

      Visible after;
      FindVisible(account, calendarId, after);
      return BuildResponse_(200, CalendarJson(account, after, true));
   }

   //------------------------------------------------------------------------
   // The events
   //------------------------------------------------------------------------

   HttpResponse
   RestApiServer::HandleMeEvents_(const Caller &caller, const AnsiString &query)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      __int64 windowStart = 0;
      __int64 windowEnd = 0;
      AnsiString startText = QueryValue(query, "start");
      AnsiString endText = QueryValue(query, "end");
      if (startText.IsEmpty() || endText.IsEmpty() || !ICalendar::ParseUtc(startText, windowStart) || !ICalendar::ParseUtc(endText, windowEnd))
         return BuildResponse_(400, "{\"error\":\"start and end are required, as UTC date-times YYYYMMDDTHHMMSSZ\"}");
      if (windowEnd <= windowStart)
         return BuildResponse_(400, "{\"error\":\"end must be after start\"}");
      if (windowEnd - windowStart > 400LL * 86400)
         return BuildResponse_(400, "{\"error\":\"the window is at most 400 days\"}");

      std::vector<Visible> visible;
      if (!VisibleCalendars(account, visible))
         return BuildResponse_(500, "{\"error\":\"the calendars could not be read\"}");

      AnsiString me = Lower(Utf8_(account->GetAddress()));
      AnsiString json = "{\"start\":\"" + ICalendar::FormatUtc(windowStart) + "\",\"end\":\"" + ICalendar::FormatUtc(windowEnd) + "\",\"events\":[";
      bool first = true;
      size_t total = 0;
      for (size_t c = 0; c < visible.size(); c++)
      {
         std::vector<CalendarObjectRecord> records;
         if (!CalendarStore::ListInRange(visible[c].calendar.accountId, visible[c].calendar.id, windowStart - 86400, windowEnd + 86400, records))
            continue;
         for (size_t r = 0; r < records.size(); r++)
         {
            if (records[r].component.CompareNoCase(_T("VEVENT")) != 0)
               continue;
            ICalComponent tree;
            AnsiString problem;
            if (!ICalendar::Parse(Utf8_(records[r].data), tree, problem))
               continue;
            std::vector<ICalInstance> instances;
            bool truncated = false;
            if (!ICalendar::Expand(tree, windowStart, windowEnd, 1000, instances, truncated, problem))
               continue;
            for (size_t i = 0; i < instances.size(); i++)
            {
               if (++total > 5000)
                  break;
               AnsiString one = InstanceJson(visible[c], records[r], tree, instances[i], me);
               if (one.IsEmpty())
                  continue;
               if (!first)
                  json += ",";
               first = false;
               json += one;
            }
         }
      }
      json += "]}";
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeEventGet_(const Caller &caller, __int64 objectId)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      Visible visible;
      CalendarObjectRecord record;
      if (!FindObject(account, objectId, visible, record))
         return BuildResponse_(404, "{\"error\":\"event not found\"}");

      ICalComponent tree;
      AnsiString problem;
      if (!ICalendar::Parse(Utf8_(record.data), tree, problem))
         return BuildResponse_(500, "{\"error\":\"the stored object cannot be read: " + JsonEscape_(problem) + "\"}");
      const ICalComponent *master = MasterOf(tree);
      if (!master)
         return BuildResponse_(500, "{\"error\":\"the stored object has no component\"}");

      AnsiString me = Lower(Utf8_(account->GetAddress()));
      ICalTime start;
      ICalTime end;
      AnsiString ignored;
      __int64 startInstant = record.start;
      __int64 endInstant = record.end;
      AnsiString tz = TzOf(*master);
      bool allDay = AllDayOf(*master);

      AnsiString json = "{\"id\":" + Int64Text(record.id) +
         ",\"calendar\":" + Int64Text(visible.calendar.id) +
         ",\"uid\":\"" + JsonEscape_(Utf8_(record.uid)) +
         "\",\"component\":\"" + JsonEscape_(master->name) +
         "\",\"start\":\"" + ICalendar::FormatUtc(startInstant) +
         "\",\"end\":\"" + ICalendar::FormatUtc(endInstant) +
         "\",\"local_start\":\"" + LocalText(tz, startInstant, allDay) +
         "\",\"local_end\":\"" + LocalText(tz, endInstant, allDay) +
         "\",\"recurring\":" + AnsiString(master->Property("RRULE") ? "true" : "false") +
         ",\"writable\":" + AnsiString(visible.writable ? "true" : "false") +
         ",\"mine\":" + AnsiString(visible.mine ? "true" : "false") +
         ",\"delegate\":" + AnsiString(!visible.mine && visible.delegate ? "true" : "false") +
         ",\"color\":\"" + JsonEscape_(Utf8_(visible.calendar.color)) + "\"," +
         ComponentFieldsJson(*master, tree, me) +
         ",\"ics\":\"" + JsonEscape_(Utf8_(record.data)) + "\"}";
      return BuildResponse_(200, json);
   }

   // The invitations that go with a change to an event with attendees: one
   // REQUEST per attendee (or CANCEL), from the organiser account.
   int
   RestApiServer::SendEventMail_(std::shared_ptr<const Account> account, const ICalComponent &tree, const AnsiString &method, AnsiString &problem)
   {
      const ICalComponent *master = MasterOf(tree);
      if (!master)
         return 0;

      AnsiString me = Lower(Utf8_(account->GetAddress()));
      AnsiString organizer = Imip::AddressOf(master->Value("ORGANIZER"));
      if (!organizer.IsEmpty() && organizer != me)
      {
         problem = "{\"error\":\"only the organiser sends invitations for this event\"}";
         return 403;
      }

      ImipMessage described;
      AnsiString ignored;
      Imip::Read(ICalendar::Serialize(tree), described, ignored);

      AnsiString text = Imip::WithMethod(tree, method);
      AnsiString subjectHead = method == "CANCEL" ? "Cancelled: " : (method == "REQUEST" ? "Invitation: " : method + ": ");
      AnsiString subject = subjectHead + ICalendar::Unescape(master->Value("SUMMARY")) + " @ " + Imip::WhenText(described.start, described.end, described.allDay);
      AnsiString body = (method == "CANCEL" ? AnsiString("This event has been cancelled.\r\n\r\n") : AnsiString("You are invited to: " + ICalendar::Unescape(master->Value("SUMMARY")) + "\r\n\r\n")) + Imip::Describe(described);
      if (method == "REQUEST")
         body += "\r\nOpen this message in a calendar-aware mail program to accept or decline.\r\n";

      int sent = 0;
      for (size_t i = 0; i < master->properties.size(); i++)
      {
         const ICalProperty &property = master->properties[i];
         if (property.name != "ATTENDEE")
            continue;
         AnsiString to = Imip::AddressOf(property.value);
         if (to.IsEmpty() || to == me)
            continue;
         AnsiString mail = Imip::BuildMail(Utf8_(FromHeader_(account)), to, subject, body, method, text, me);
         String toWide;
         Unicode::MultiByteToWide(to, toWide);
         AnsiString refusal;
         int status = QueueRawMessage_(account, toWide, mail, refusal);
         if (status != 0)
         {
            problem = refusal;
            return status;
         }
         sent++;
      }
      LOG_DEBUG("Calendar: " + account->GetAddress() + " sent " + String(method) + " for " + String(master->Value("UID")) + " to " + String(IntText(sent)) + " attendee(s).");
      return 0;
   }

   HttpResponse
   RestApiServer::HandleMeEventCreate_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      JsonValue document;
      AnsiString problem;
      if (!ParseBody(requestBody, document, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      Fields fields;
      if (!ReadFields(document, fields, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
      if (!fields.hasSummary || fields.summary.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"summary is required\"}");
      if (!fields.hasStart || fields.start.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"start is required\"}");

      Visible visible;
      const JsonValue *calendarValue = document.Get("calendar");
      if (calendarValue && calendarValue->IsNumber() && calendarValue->AsNumber() > 0)
      {
         if (!FindVisible(account, calendarValue->AsInt64(), visible))
            return BuildResponse_(404, "{\"error\":\"calendar not found\"}");
      }
      else
      {
         std::vector<Visible> all;
         if (!VisibleCalendars(account, all) || all.empty())
            return BuildResponse_(500, "{\"error\":\"the calendars could not be read\"}");
         visible = all[0];
      }
      if (!visible.writable)
         return BuildResponse_(403, "{\"error\":\"the calendar is shared with you read-only\"}");

      if (!fields.hasEnd || fields.end.IsEmpty())
      {
         // An hour, or the day, when the page did not say.
         fields.hasEnd = true;
         bool isDate, utc;
         int y, m, d, hh, mm, ss;
         if (ParseWall(fields.start, isDate, utc, y, m, d, hh, mm, ss) && !(isDate || fields.allDay))
         {
            __int64 instant = 0;
            AnsiString tz = fields.hasTz ? fields.tz : AnsiString("UTC");
            if (!ICalendar::LocalToInstant(tz, y, m, d, hh, mm, ss, instant))
               ICalendar::LocalToInstant("UTC", y, m, d, hh, mm, ss, instant);
            fields.end = LocalText(tz, instant + 3600, false);
         }
         else
            fields.end = "";
      }

      ICalComponent tree = NewCalendar();
      ICalComponent event;
      event.name = "VEVENT";
      SetPlain(event, "UID", NewUid(account));
      SetPlain(event, "CREATED", ICalendar::FormatUtc(NowUtc()));
      SetPlain(event, "SEQUENCE", "0");
      if (!ApplyFields(event, fields, fields.hasTz ? fields.tz : AnsiString(""), fields.allDay, Lower(Utf8_(account->GetAddress())), OrganizerName(account), problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
      if (fields.allDay && fields.hasEnd && fields.end.IsEmpty())
      {
         // An all-day event of one day: DTEND is the next day, exclusive.
         bool isDate, utc;
         int y, m, d, hh, mm, ss;
         if (ParseWall(fields.start, isDate, utc, y, m, d, hh, mm, ss))
         {
            __int64 instant = 0;
            ICalendar::LocalToInstant("UTC", y, m, d, 0, 0, 0, instant);
            ICalProperty end;
            if (TimeProperty("DTEND", LocalText("UTC", instant + 86400, true), true, "", end, problem))
               SetProperty(event, end);
         }
      }
      tree.children.push_back(event);

      CalendarObjectRecord record;
      if (!Store(visible, tree, record, true, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      AnsiString mailProblem;
      int sent = 0;
      if (GetJsonBoolValue_(requestBody, "invite", false) && fields.hasAttendees && !fields.attendees.empty())
      {
         int status = SendEventMail_(account, tree, "REQUEST", mailProblem);
         if (status != 0)
            return BuildResponse_(status, mailProblem);
         sent = static_cast<int>(fields.attendees.size());
      }

      LOG_DEBUG("Calendar: " + account->GetAddress() + " created " + record.uid + " in " + visible.calendar.name + ".");
      return BuildResponse_(201, "{\"id\":" + Int64Text(record.id) + ",\"uid\":\"" + JsonEscape_(Utf8_(record.uid)) + "\",\"calendar\":" + Int64Text(visible.calendar.id) + ",\"invited\":" + IntText(sent) + "}");
   }

   HttpResponse
   RestApiServer::HandleMeEventUpdate_(const Caller &caller, __int64 objectId, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      Visible visible;
      CalendarObjectRecord record;
      if (!FindObject(account, objectId, visible, record))
         return BuildResponse_(404, "{\"error\":\"event not found\"}");
      if (!visible.writable)
         return BuildResponse_(403, "{\"error\":\"the calendar is shared with you read-only\"}");

      JsonValue document;
      AnsiString problem;
      if (!ParseBody(requestBody, document, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
      Fields fields;
      if (!ReadFields(document, fields, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
      if (fields.hasSummary && fields.summary.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"summary cannot be empty\"}");

      ICalComponent tree;
      if (!ICalendar::Parse(Utf8_(record.data), tree, problem))
         return BuildResponse_(500, "{\"error\":\"the stored object cannot be read\"}");
      ICalComponent *master = MasterOf(tree);
      if (!master)
         return BuildResponse_(500, "{\"error\":\"the stored object has no component\"}");

      AnsiString me = Lower(Utf8_(account->GetAddress()));
      AnsiString organizer = Imip::AddressOf(master->Value("ORGANIZER"));
      bool organiserIsOther = !organizer.IsEmpty() && organizer != me;

      // The calendar moved to, when the body names another.
      Visible target = visible;
      const JsonValue *calendarValue = document.Get("calendar");
      if (calendarValue && calendarValue->IsNumber() && calendarValue->AsInt64() != visible.calendar.id)
      {
         if (!FindVisible(account, calendarValue->AsInt64(), target))
            return BuildResponse_(404, "{\"error\":\"calendar not found\"}");
         if (!target.writable)
            return BuildResponse_(403, "{\"error\":\"the calendar is shared with you read-only\"}");
      }

      AnsiString instanceText = StringOf(document.Get("instance"));
      __int64 instant = 0;
      if (!instanceText.IsEmpty())
      {
         if (!ICalendar::ParseUtc(instanceText, instant))
            return BuildResponse_(400, "{\"error\":\"instance must be the occurrence's recurrence_id, YYYYMMDDTHHMMSSZ\"}");
         if (!master->Property("RRULE") && !master->Property("RDATE"))
            return BuildResponse_(400, "{\"error\":\"the event does not recur; change it without instance\"}");

         ICalInstance found;
         if (!FindInstance(tree, instant, found))
            return BuildResponse_(404, "{\"error\":\"no occurrence starts then\"}");

         bool allDay = AllDayOf(*master);
         AnsiString tz = TzOf(*master);
         bool had = false;
         for (size_t i = 0; i < tree.children.size() && !had; i++)
            had = tree.children[i].Property("RECURRENCE-ID") != nullptr && tree.children[i].name == master->name && FindInstance(tree, instant, found) && found.overridden;
         ICalComponent &component = OverrideFor(tree, instant, allDay, tz);
         // OverrideFor appends the override when there is none yet, and the
         // append can move every component: master is found again rather than
         // read through a pointer into the old storage. component is the one
         // just appended and stays where it is, since nothing is appended after.
         master = MasterOf(tree);
         if (!found.overridden)
            GiveInstanceTimes(component, found, tz, allDay);
         Fields one = fields;
         one.hasRrule = false;   // a rule belongs to the series
         if (!ApplyFields(component, one, tz, allDay, me, OrganizerName(account), problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
      }
      else
      {
         AnsiString tz = TzOf(*master);
         bool allDay = AllDayOf(*master);
         const ICalComponent before = *master;
         if (!ApplyFields(*master, fields, tz, allDay, me, OrganizerName(account), problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
         // The overrides follow a change of the kind or zone; otherwise they keep their own times.
         if (fields.hasAllDay || fields.hasTz)
         {
            for (size_t i = 0; i < tree.children.size();)
            {
               if (tree.children[i].name == master->name && tree.children[i].Property("RECURRENCE-ID"))
                  tree.children.erase(tree.children.begin() + i);
               else
                  i++;
            }
            master = MasterOf(tree);
         }
         else
         {
            for (size_t i = 0; i < tree.children.size(); i++)
            {
               ICalComponent &occurrence = tree.children[i];
               if (occurrence.name != before.name || !occurrence.Property("RECURRENCE-ID"))
                  continue;
               Fields follow = FieldsToFollow(fields, before, occurrence);
               if (follow.hasSummary || follow.hasLocation || follow.hasDescription || follow.hasAlarm || follow.hasPriority || follow.hasStatus || follow.hasAttendees)
               {
                  if (!ApplyFields(occurrence, follow, tz, allDay, me, OrganizerName(account), problem))
                     return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
               }
            }
         }
      }

      bool hasAttendees = master->Property("ATTENDEE") != nullptr;
      if (hasAttendees && !organiserIsOther)
         SetPlain(*master, "SEQUENCE", IntText(NextSequence(*master)));

      if (target.calendar.id != visible.calendar.id)
      {
         // Moved: stored anew in the other calendar, and the old row gone.
         CalendarObjectRecord made;
         if (!Store(target, tree, made, true, problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
         __int64 token = 0;
         {
            ResourceCalendarGuard guard(visible.calendar.accountId);
            // Moved to another account's calendar, the booking is no longer
            // the room's, and its record goes; moved between two calendars of
            // the same account, it is, and the record stays.
            if (CalendarStore::Delete(visible.calendar.accountId, visible.calendar.id, record.id, token) &&
                target.calendar.accountId != visible.calendar.accountId)
               ResourceBooking::ForgetDecision(visible.calendar.accountId, record.uid);
         }
         record = made;
         visible = target;
      }
      else if (!Store(visible, tree, record, false, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      int sent = 0;
      if (GetJsonBoolValue_(requestBody, "invite", false) && hasAttendees && !organiserIsOther)
      {
         AnsiString mailProblem;
         int status = SendEventMail_(account, tree, "REQUEST", mailProblem);
         if (status != 0)
            return BuildResponse_(status, mailProblem);
         sent = 1;
      }

      return BuildResponse_(200, "{\"id\":" + Int64Text(record.id) + ",\"uid\":\"" + JsonEscape_(Utf8_(record.uid)) + "\",\"calendar\":" + Int64Text(visible.calendar.id) + ",\"invited\":" + AnsiString(sent ? "true" : "false") + "}");
   }

   HttpResponse
   RestApiServer::HandleMeEventDelete_(const Caller &caller, __int64 objectId, const AnsiString &query)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      Visible visible;
      CalendarObjectRecord record;
      if (!FindObject(account, objectId, visible, record))
         return BuildResponse_(404, "{\"error\":\"event not found\"}");
      if (!visible.writable)
         return BuildResponse_(403, "{\"error\":\"the calendar is shared with you read-only\"}");

      AnsiString problem;
      ICalComponent tree;
      if (!ICalendar::Parse(Utf8_(record.data), tree, problem))
         return BuildResponse_(500, "{\"error\":\"the stored object cannot be read\"}");
      ICalComponent *master = MasterOf(tree);
      if (!master)
         return BuildResponse_(500, "{\"error\":\"the stored object has no component\"}");

      bool notify = QueryValue(query, "notify") == "1" || QueryValue(query, "notify") == "true";
      AnsiString instanceText = QueryValue(query, "instance");
      if (!instanceText.IsEmpty())
      {
         __int64 instant = 0;
         if (!ICalendar::ParseUtc(instanceText, instant))
            return BuildResponse_(400, "{\"error\":\"instance must be the occurrence's recurrence_id, YYYYMMDDTHHMMSSZ\"}");
         if (!master->Property("RRULE") && !master->Property("RDATE"))
            return BuildResponse_(400, "{\"error\":\"the event does not recur; delete it without instance\"}");
         ICalInstance found;
         if (!FindInstance(tree, instant, found))
            return BuildResponse_(404, "{\"error\":\"no occurrence starts then\"}");

         RemoveOverride(tree, instant);
         master = MasterOf(tree);
         AddExdate(*master, instant, AllDayOf(*master), TzOf(*master));
         SetPlain(*master, "DTSTAMP", ICalendar::FormatUtc(NowUtc()));
         if (!Store(visible, tree, record, false, problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
         return BuildResponse_(200, "{\"removed\":\"instance\",\"id\":" + Int64Text(record.id) + "}");
      }

      if (notify && master->Property("ATTENDEE"))
      {
         SetPlain(*master, "SEQUENCE", IntText(NextSequence(*master)));
         SetPlain(*master, "STATUS", "CANCELLED");
         AnsiString mailProblem;
         int status = SendEventMail_(account, tree, "CANCEL", mailProblem);
         if (status != 0)
            return BuildResponse_(status, mailProblem);
      }

      __int64 token = 0;
      bool deleted = false;
      {
         ResourceCalendarGuard guard(visible.calendar.accountId);
         deleted = CalendarStore::Delete(visible.calendar.accountId, visible.calendar.id, record.id, token);
         if (deleted)
            ResourceBooking::ForgetDecision(visible.calendar.accountId, record.uid);
      }
      if (!deleted)
         return BuildResponse_(404, "{\"error\":\"event not found\"}");
      LOG_DEBUG("Calendar: " + account->GetAddress() + " deleted " + record.uid + ".");
      return BuildResponse_(200, "{\"removed\":\"event\",\"id\":" + Int64Text(record.id) + "}");
   }

   //------------------------------------------------------------------------
   // The tasks
   //------------------------------------------------------------------------

   HttpResponse
   RestApiServer::HandleMeTasks_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      std::vector<Visible> visible;
      if (!VisibleCalendars(account, visible))
         return BuildResponse_(500, "{\"error\":\"the calendars could not be read\"}");

      AnsiString me = Lower(Utf8_(account->GetAddress()));
      std::vector<std::pair<AnsiString, AnsiString>> rows;   // sort key, json
      for (size_t c = 0; c < visible.size(); c++)
      {
         std::vector<CalendarObjectRecord> records;
         if (!CalendarStore::List(visible[c].calendar.accountId, visible[c].calendar.id, records))
            continue;
         for (size_t r = 0; r < records.size(); r++)
         {
            if (records[r].component.CompareNoCase(_T("VTODO")) != 0)
               continue;
            ICalComponent tree;
            AnsiString problem;
            if (!ICalendar::Parse(Utf8_(records[r].data), tree, problem))
               continue;
            AnsiString json = TaskJson(visible[c], records[r], tree, me);
            if (json.IsEmpty())
               continue;
            const ICalComponent *master = MasterOf(tree);
            AnsiString due = "9";
            if (const ICalProperty *property = master->Property("DUE"))
            {
               ICalTime time;
               AnsiString ignored;
               if (ICalendar::ParseTime(*property, &tree, time, ignored))
                  due = ICalendar::FormatUtc(time.instant);
            }
            rows.push_back(std::make_pair(due + " " + Lower(ICalendar::Unescape(master->Value("SUMMARY"))), json));
         }
      }
      std::sort(rows.begin(), rows.end());

      AnsiString json = "{\"tasks\":[";
      for (size_t i = 0; i < rows.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += rows[i].second;
      }
      json += "]}";
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeTaskCreate_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      JsonValue document;
      AnsiString problem;
      if (!ParseBody(requestBody, document, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
      Fields fields;
      if (!ReadFields(document, fields, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
      if (!fields.hasSummary || fields.summary.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"summary is required\"}");

      Visible visible;
      const JsonValue *calendarValue = document.Get("calendar");
      if (calendarValue && calendarValue->IsNumber() && calendarValue->AsNumber() > 0)
      {
         if (!FindVisible(account, calendarValue->AsInt64(), visible))
            return BuildResponse_(404, "{\"error\":\"calendar not found\"}");
      }
      else
      {
         std::vector<Visible> all;
         if (!VisibleCalendars(account, all) || all.empty())
            return BuildResponse_(500, "{\"error\":\"the calendars could not be read\"}");
         visible = all[0];
      }
      if (!visible.writable)
         return BuildResponse_(403, "{\"error\":\"the calendar is shared with you read-only\"}");

      ICalComponent tree = NewCalendar();
      ICalComponent todo;
      todo.name = "VTODO";
      SetPlain(todo, "UID", NewUid(account));
      SetPlain(todo, "CREATED", ICalendar::FormatUtc(NowUtc()));
      SetPlain(todo, "SEQUENCE", "0");
      SetPlain(todo, "STATUS", "NEEDS-ACTION");
      // A task's date is its DUE; the page sends it as due, or as end.
      if (!fields.hasDue && fields.hasEnd)
      {
         fields.hasDue = true;
         fields.due = fields.end;
         fields.hasEnd = false;
      }
      fields.hasStart = false;
      bool allDay = fields.hasAllDay ? fields.allDay : (fields.hasDue && fields.due.GetLength() == 10);
      fields.hasAllDay = false;
      if (!ApplyFields(todo, fields, fields.hasTz ? fields.tz : AnsiString(""), allDay, "", "", problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
      tree.children.push_back(todo);

      CalendarObjectRecord record;
      if (!Store(visible, tree, record, true, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      AnsiString me = Lower(Utf8_(account->GetAddress()));
      return BuildResponse_(201, TaskJson(visible, record, tree, me));
   }

   HttpResponse
   RestApiServer::HandleMeTaskUpdate_(const Caller &caller, __int64 objectId, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      Visible visible;
      CalendarObjectRecord record;
      if (!FindObject(account, objectId, visible, record) || record.component.CompareNoCase(_T("VTODO")) != 0)
         return BuildResponse_(404, "{\"error\":\"task not found\"}");
      if (!visible.writable)
         return BuildResponse_(403, "{\"error\":\"the calendar is shared with you read-only\"}");

      JsonValue document;
      AnsiString problem;
      if (!ParseBody(requestBody, document, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
      Fields fields;
      if (!ReadFields(document, fields, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
      if (fields.hasSummary && fields.summary.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"summary cannot be empty\"}");

      ICalComponent tree;
      if (!ICalendar::Parse(Utf8_(record.data), tree, problem))
         return BuildResponse_(500, "{\"error\":\"the stored object cannot be read\"}");
      ICalComponent *master = MasterOf(tree);
      if (!master)
         return BuildResponse_(500, "{\"error\":\"the stored object has no component\"}");

      if (!fields.hasDue && fields.hasEnd)
      {
         fields.hasDue = true;
         fields.due = fields.end;
      }
      fields.hasEnd = false;
      fields.hasStart = false;
      bool allDay = fields.hasAllDay ? fields.allDay : (fields.hasDue ? fields.due.GetLength() == 10 : AllDayOf(*master));
      fields.hasAllDay = false;
      AnsiString tz = fields.hasTz ? fields.tz : AnsiString("");
      if (tz.IsEmpty())
      {
         if (const ICalProperty *due = master->Property("DUE"))
            tz = due->Parameter("TZID");
      }
      if (!ApplyFields(*master, fields, tz, allDay, "", "", problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
      SetPlain(*master, "SEQUENCE", IntText(NextSequence(*master)));

      Visible target = visible;
      const JsonValue *calendarValue = document.Get("calendar");
      if (calendarValue && calendarValue->IsNumber() && calendarValue->AsInt64() != visible.calendar.id)
      {
         if (!FindVisible(account, calendarValue->AsInt64(), target))
            return BuildResponse_(404, "{\"error\":\"calendar not found\"}");
         if (!target.writable)
            return BuildResponse_(403, "{\"error\":\"the calendar is shared with you read-only\"}");
         CalendarObjectRecord made;
         if (!Store(target, tree, made, true, problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
         __int64 token = 0;
         {
            ResourceCalendarGuard guard(visible.calendar.accountId);
            CalendarStore::Delete(visible.calendar.accountId, visible.calendar.id, record.id, token);
         }
         record = made;
         visible = target;
      }
      else if (!Store(visible, tree, record, false, problem))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

      AnsiString me = Lower(Utf8_(account->GetAddress()));
      return BuildResponse_(200, TaskJson(visible, record, tree, me));
   }

   HttpResponse
   RestApiServer::HandleMeTaskDelete_(const Caller &caller, __int64 objectId)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      Visible visible;
      CalendarObjectRecord record;
      if (!FindObject(account, objectId, visible, record) || record.component.CompareNoCase(_T("VTODO")) != 0)
         return BuildResponse_(404, "{\"error\":\"task not found\"}");
      if (!visible.writable)
         return BuildResponse_(403, "{\"error\":\"the calendar is shared with you read-only\"}");

      __int64 token = 0;
      if (!CalendarStore::Delete(visible.calendar.accountId, visible.calendar.id, record.id, token))
         return BuildResponse_(404, "{\"error\":\"task not found\"}");
      return BuildResponse_(200, "{\"removed\":true,\"id\":" + Int64Text(record.id) + "}");
   }

   //------------------------------------------------------------------------
   // The invitation a message carries
   //------------------------------------------------------------------------

   // The stored copy of the invitation's event in the account's own
   // calendars, if any.
   bool
   RestApiServer::FindStoredEvent_(std::shared_ptr<const Account> account, const AnsiString &uid, CalendarRecord &calendar, CalendarObjectRecord &record)
   {
      if (uid.IsEmpty())
         return false;
      String wide;
      Unicode::MultiByteToWide(uid, wide);
      std::vector<CalendarRecord> calendars;
      if (!CalendarStore::ListCalendars(account->GetID(), calendars))
         return false;
      for (size_t i = 0; i < calendars.size(); i++)
      {
         if (CalendarStore::FindByUid(account->GetID(), calendars[i].id, wide, record))
         {
            calendar = calendars[i];
            return true;
         }
      }
      return false;
   }

   HttpResponse
   RestApiServer::HandleMeMessageInvitation_(const Caller &caller, __int64 messageId)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      if (!PersistentMessage::ReadObject(message, messageId) || message->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"message not found\"}");
      std::shared_ptr<IMAPFolder> folder = FindReadableFolder_(account, message->GetFolderID());
      if (!folder || folder->GetAccountID() != message->GetAccountID())
         return BuildResponse_(404, "{\"error\":\"message not found\"}");

      AnsiString calendarText;
      if (!Imip::FindInFile(MessageFile_(message), calendarText))
         return BuildResponse_(404, "{\"error\":\"the message carries no invitation\"}");

      ImipMessage invitation;
      AnsiString problem;
      if (!Imip::Read(calendarText, invitation, problem))
         return BuildResponse_(422, "{\"error\":\"the calendar part cannot be read: " + JsonEscape_(problem) + "\"}");

      AnsiString me = Lower(Utf8_(account->GetAddress()));
      const ICalComponent *master = MasterOf(invitation.tree);

      CalendarRecord calendar;
      CalendarObjectRecord stored;
      bool inCalendar = FindStoredEvent_(account, invitation.uid, calendar, stored);
      AnsiString storedPartstat;
      AnsiString storedStatus;
      // Whether the calendar holds what the message says: a CANCEL's event
      // marked cancelled, a REPLY's (first) answer recorded, a REQUEST at this
      // version or a later one. The delivery filer applies a message only
      // from a sender it verified (Imip::FileOnDelivery); any other is an
      // ordinary message and the stored event is as it was, so the page says
      // the calendar changed only when this says it did.
      bool calendarAgrees = false;
      if (inCalendar)
      {
         ICalComponent storedTree;
         AnsiString ignored;
         if (ICalendar::Parse(Utf8_(stored.data), storedTree, ignored))
         {
            if (const ICalComponent *storedMaster = MasterOf(storedTree))
            {
               storedPartstat = MyPartstat(*storedMaster, me);
               storedStatus = Trimmed(storedMaster->Value("STATUS"));
               storedStatus.ToUpper();

               if (invitation.method == "CANCEL")
                  calendarAgrees = storedStatus == "CANCELLED";
               else if (invitation.method == "REPLY" && !invitation.attendees.empty())
               {
                  const ImipAttendee &answer = invitation.attendees[0];
                  const std::vector<AnsiString> listed = Imip::ListedAddressesFor(storedTree, answer.address,
                     [](const AnsiString &one, const AnsiString &other) { return Imip::SameMailbox(one, other); });
                  for (size_t i = 0; i < listed.size() && !calendarAgrees; i++)
                     calendarAgrees = MyPartstat(*storedMaster, listed[i]) == answer.partstat;
               }
               else if (invitation.method == "REQUEST" && master)
               {
                  // A later version stored, or this one: what it says - when,
                  // what, where, how often, whose - is what the calendar has.
                  // A later version only when the server verified who sent it:
                  // a copy filed unverified (Imip::FiledUnverified) is not this
                  // message's, whatever SEQUENCE its writer gave it.
                  const int storedSequence = SequenceOf(*storedMaster);
                  calendarAgrees = (storedSequence > invitation.sequence && !Imip::FiledUnverified(storedTree)) ||
                     (storedSequence == invitation.sequence && stored.start == invitation.start && stored.end == invitation.end &&
                      storedMaster->Value("SUMMARY") == master->Value("SUMMARY") && storedMaster->Value("LOCATION") == master->Value("LOCATION") &&
                      storedMaster->Value("RRULE") == master->Value("RRULE") &&
                      Imip::AddressOf(storedMaster->Value("ORGANIZER")) == invitation.organizer);
               }
            }
         }
      }

      AnsiString json = "{\"method\":\"" + JsonEscape_(invitation.method) +
         "\",\"component\":\"" + JsonEscape_(invitation.component) +
         "\",\"uid\":\"" + JsonEscape_(invitation.uid) +
         "\",\"start\":\"" + ICalendar::FormatUtc(invitation.start) +
         "\",\"end\":\"" + ICalendar::FormatUtc(invitation.end) +
         "\",\"local_start\":\"" + LocalText(invitation.tzid, invitation.start, invitation.allDay) +
         "\",\"local_end\":\"" + LocalText(invitation.tzid, invitation.end, invitation.allDay) +
         "\",\"recurring\":" + AnsiString(invitation.recurring ? "true" : "false") +
         ",\"in_calendar\":" + AnsiString(inCalendar ? "true" : "false") +
         ",\"event_id\":" + Int64Text(inCalendar ? stored.id : 0) +
         ",\"calendar\":" + Int64Text(inCalendar ? calendar.id : 0) +
         ",\"stored_partstat\":\"" + JsonEscape_(storedPartstat) +
         "\",\"stored_status\":\"" + JsonEscape_(storedStatus) +
         "\",\"calendar_agrees\":" + AnsiString(calendarAgrees ? "true" : "false") +
         ",\"i_am_organizer\":" + AnsiString(invitation.organizer == me ? "true" : "false") + ",";
      json += master ? ComponentFieldsJson(*master, invitation.tree, me) : AnsiString("\"summary\":\"\"");
      json += "}";
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeMessageInvitationReply_(const Caller &caller, __int64 messageId, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
      if (!PersistentMessage::ReadObject(message, messageId) || message->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"message not found\"}");
      std::shared_ptr<IMAPFolder> folder = FindReadableFolder_(account, message->GetFolderID());
      if (!folder || folder->GetAccountID() != message->GetAccountID())
         return BuildResponse_(404, "{\"error\":\"message not found\"}");

      AnsiString calendarText;
      if (!Imip::FindInFile(MessageFile_(message), calendarText))
         return BuildResponse_(404, "{\"error\":\"the message carries no invitation\"}");
      ImipMessage invitation;
      AnsiString problem;
      if (!Imip::Read(calendarText, invitation, problem))
         return BuildResponse_(422, "{\"error\":\"the calendar part cannot be read: " + JsonEscape_(problem) + "\"}");
      if (invitation.uid.IsEmpty())
         return BuildResponse_(422, "{\"error\":\"the invitation has no UID\"}");

      AnsiString action = Lower(Trimmed(JsonUtf8Value_(requestBody, "action").IsEmpty() ? AnsiString("") : Utf8_(JsonUtf8Value_(requestBody, "action"))));
      AnsiString me = Lower(Utf8_(account->GetAddress()));
      AnsiString myName = OrganizerName(account);

      CalendarRecord calendar;
      CalendarObjectRecord stored;
      bool inCalendar = FindStoredEvent_(account, invitation.uid, calendar, stored);

      if (action == "remove")
      {
         if (!inCalendar)
            return BuildResponse_(200, "{\"removed\":false}");
         __int64 token = 0;
         if (!CalendarStore::Delete(account->GetID(), calendar.id, stored.id, token))
            return BuildResponse_(500, "{\"error\":\"the event could not be removed\"}");
         return BuildResponse_(200, "{\"removed\":true}");
      }

      AnsiString partstat;
      if (action == "accept")
         partstat = "ACCEPTED";
      else if (action == "tentative")
         partstat = "TENTATIVE";
      else if (action == "decline")
         partstat = "DECLINED";
      else if (action != "counter")
         return BuildResponse_(400, "{\"error\":\"action is accept, tentative, decline, remove or counter\"}");

      if (invitation.organizer == me)
         return BuildResponse_(400, "{\"error\":\"you are the organiser of this event; answers are the attendees'\"}");

      if (action == "counter")
      {
         JsonValue document;
         if (!ParseBody(requestBody, document, problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
         Fields fields;
         if (!ReadFields(document, fields, problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
         if (!fields.hasStart || fields.start.IsEmpty())
            return BuildResponse_(400, "{\"error\":\"start is required to propose a new time\"}");
         if (invitation.organizer.IsEmpty())
            return BuildResponse_(400, "{\"error\":\"the invitation names no organiser to answer\"}");

         ICalComponent counter = invitation.tree;
         ICalComponent *master = MasterOf(counter);
         if (!master)
            return BuildResponse_(422, "{\"error\":\"the invitation has no event\"}");
         Fields times;
         times.hasStart = fields.hasStart; times.start = fields.start;
         times.hasEnd = fields.hasEnd; times.end = fields.end;
         times.hasTz = fields.hasTz; times.tz = fields.tz;
         times.hasAllDay = fields.hasAllDay; times.allDay = fields.allDay;
         if (!ApplyFields(*master, times, TzOf(*master), AllDayOf(*master), "", "", problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");
         AnsiString comment = Utf8_(JsonUtf8Value_(requestBody, "comment"));
         if (!comment.IsEmpty())
            SetText(*master, "COMMENT", comment);

         ImipMessage proposed;
         AnsiString ignored;
         Imip::Read(ICalendar::Serialize(counter), proposed, ignored);
         AnsiString text = Imip::WithMethod(counter, "COUNTER");
         AnsiString subject = "New time proposed: " + ICalendar::Unescape(master->Value("SUMMARY")) + " @ " + Imip::WhenText(proposed.start, proposed.end, proposed.allDay);
         AnsiString body = (myName.IsEmpty() ? me : myName) + " proposes a new time for this event.\r\n\r\n" + Imip::Describe(proposed) + (comment.IsEmpty() ? AnsiString("") : "\r\n" + comment + "\r\n");
         AnsiString mail = Imip::BuildMail(Utf8_(FromHeader_(account)), invitation.organizer, subject, body, "COUNTER", text, me);
         String to;
         Unicode::MultiByteToWide(invitation.organizer, to);
         AnsiString refusal;
         int status = QueueRawMessage_(account, to, mail, refusal);
         if (status != 0)
            return BuildResponse_(status, refusal);
         return BuildResponse_(200, "{\"countered\":true,\"to\":\"" + JsonEscape_(invitation.organizer) + "\"}");
      }

      // The event kept, with the answer on it, by the one path the delivery
      // filer takes (Imip::KeepInvitation): the invitation's copy replaces what
      // was stored when it is the same or a later sequence, an older one does
      // not undo a newer, and a REQUEST already filed on delivery is this same
      // object updated, never a second one.
      Visible target;
      if (!Imip::KeepInvitation(account->GetID(), invitation, true, invitation.method == "CANCEL", me, myName, partstat, target.calendar, stored, problem))
         return BuildResponse_(422, "{\"error\":\"the event could not be stored: " + JsonEscape_(problem) + "\"}");

      bool replied = false;
      AnsiString repliedTo;
      if (!invitation.organizer.IsEmpty() && invitation.organizer != me && invitation.method != "CANCEL")
      {
         AnsiString replyText = Imip::ReplyObject(invitation, me, myName, partstat);
         AnsiString word = partstat == "ACCEPTED" ? "Accepted" : (partstat == "TENTATIVE" ? "Tentative" : "Declined");
         AnsiString subject = word + ": " + ICalendar::Unescape(invitation.summary) + " @ " + Imip::WhenText(invitation.start, invitation.end, invitation.allDay);
         AnsiString body = (myName.IsEmpty() ? me : myName + " <" + me + ">") + " has " + Lower(word) + (partstat == "TENTATIVE" ? "ly accepted" : "") + " this invitation.\r\n\r\n" + Imip::Describe(invitation);
         AnsiString comment = Utf8_(JsonUtf8Value_(requestBody, "comment"));
         if (!comment.IsEmpty())
            body += "\r\n" + comment + "\r\n";
         AnsiString mail = Imip::BuildMail(Utf8_(FromHeader_(account)), invitation.organizer, subject, body, "REPLY", replyText, me);
         String to;
         Unicode::MultiByteToWide(invitation.organizer, to);
         AnsiString refusal;
         int status = QueueRawMessage_(account, to, mail, refusal);
         if (status == 0)
         {
            replied = true;
            repliedTo = invitation.organizer;
         }
         else
            LOG_DEBUG("Calendar: the reply to " + String(invitation.organizer) + " could not be queued: " + String(refusal));
      }

      LOG_DEBUG("Calendar: " + account->GetAddress() + " answered " + String(partstat) + " to " + String(invitation.uid) + ".");
      return BuildResponse_(200, "{\"stored\":true,\"event_id\":" + Int64Text(stored.id) + ",\"calendar\":" + Int64Text(target.calendar.id) +
         ",\"partstat\":\"" + partstat + "\",\"replied\":" + AnsiString(replied ? "true" : "false") + ",\"to\":\"" + JsonEscape_(repliedTo) + "\"}");
   }

   HttpResponse
   RestApiServer::HandleMeCalendarGet_(const Caller &caller, __int64 calendarId)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      Visible v;
      if (!FindVisible(account, calendarId, v))
         return BuildResponse_(404, "{\"error\":\"calendar not found\"}");
      return BuildResponse_(200, CalendarJson(account, v, true));
   }

   //------------------------------------------------------------------------
   // Free-busy: when an account of the caller's domain is busy
   //------------------------------------------------------------------------

   HttpResponse
   RestApiServer::HandleMeFreeBusy_(const Caller &caller, const AnsiString &query)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      AnsiString addressText = Lower(Trimmed(QueryValue(query, "address")));
      if (addressText.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"address is required: an account of your domain\"}");

      __int64 from = 0;
      __int64 to = 0;
      if (!FreeBusy::ParseInstant(QueryValue(query, "from"), from) || !FreeBusy::ParseInstant(QueryValue(query, "to"), to))
         return BuildResponse_(400, "{\"error\":\"from and to are required: YYYY-MM-DD, YYYY-MM-DDTHH:MM[:SS] with Z or an offset, or YYYYMMDDTHHMMSSZ\"}");
      if (to <= from)
         return BuildResponse_(400, "{\"error\":\"to must be after from\"}");
      if (to - from > static_cast<__int64>(FreeBusy::MaximumWindowDays) * 86400)
         return BuildResponse_(400, "{\"error\":\"the window is at most 62 days\"}");

      // One answer for an address that is not an active account of the
      // caller's domain, for one that does not exist, and for one that chose
      // not to be shown, so the route enumerates nothing. An alias domain's
      // spelling of an address is resolved first.
      const char *notShown = "{\"error\":\"no account of your domain has that address, or it does not show when it is busy\"}";
      String wanted;
      Unicode::MultiByteToWide(addressText, wanted);
      if (!StringParser::IsValidEmailAddress(wanted))
         return BuildResponse_(404, notShown);
      std::shared_ptr<DomainAliases> aliases = ObjectCache::Instance()->GetDomainAliases();
      String resolved = aliases ? aliases->ApplyAliasesOnAddress(wanted) : wanted;
      resolved.ToLower();
      std::shared_ptr<const Account> other = CacheContainer::Instance()->GetAccount(resolved);
      if (!other || !other->GetActive() || other->GetDomainID() != account->GetDomainID())
         return BuildResponse_(404, notShown);

      String choice;
      if (!ReadPreference_(other->GetID(), _T("freebusy"), choice))
         return BuildResponse_(500, "{\"error\":\"the preferences could not be read\"}");
      choice.TrimLeft();
      choice.TrimRight();
      choice.ToLower();
      if (choice == _T("nobody"))
         return BuildResponse_(404, notShown);

      AnsiString otherAddress = Lower(Utf8_(other->GetAddress()));
      std::vector<FreeBusyPeriod> periods;
      if (!FreeBusy::OfAccount(other->GetID(), otherAddress, from, to, periods))
         return BuildResponse_(500, "{\"error\":\"the calendars could not be read\"}");

      AnsiString json = "{\"address\":\"" + JsonEscape_(otherAddress) + "\",\"from\":\"" + ICalendar::FormatUtc(from) + "\",\"to\":\"" + ICalendar::FormatUtc(to) + "\",\"busy\":[";
      for (size_t i = 0; i < periods.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += "{\"start\":\"" + ICalendar::FormatUtc(periods[i].start) + "\",\"end\":\"" + ICalendar::FormatUtc(periods[i].end) +
            "\",\"kind\":\"" + AnsiString(periods[i].tentative ? "tentative" : "busy") + "\"}";
      }
      json += "]}";
      return BuildResponse_(200, json);
   }

   //------------------------------------------------------------------------
   // An invitation in a calendar answered: by its owner, or by a delegate
   //------------------------------------------------------------------------

   HttpResponse
   RestApiServer::HandleMeEventReply_(const Caller &caller, __int64 objectId, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      Visible visible;
      CalendarObjectRecord record;
      if (!FindObject(account, objectId, visible, record))
         return BuildResponse_(404, "{\"error\":\"event not found\"}");
      if (!visible.mine && !visible.delegate)
         return BuildResponse_(403, "{\"error\":\"only the calendar's owner, or a delegate the owner named, answers an invitation in it\"}");

      AnsiString partstat = Trimmed(Utf8_(JsonUtf8Value_(requestBody, "partstat")));
      partstat.ToUpper();
      if (partstat != "ACCEPTED" && partstat != "TENTATIVE" && partstat != "DECLINED")
         return BuildResponse_(400, "{\"error\":\"partstat is ACCEPTED, TENTATIVE or DECLINED\"}");

      std::shared_ptr<const Account> owner = visible.mine ? account : CacheContainer::Instance()->GetAccount(visible.calendar.accountId);
      if (!owner)
         return BuildResponse_(500, "{\"error\":\"the calendar's owner could not be read\"}");

      AnsiString problem;
      ICalComponent tree;
      if (!ICalendar::Parse(Utf8_(record.data), tree, problem))
         return BuildResponse_(500, "{\"error\":\"the stored object cannot be read\"}");
      const ICalComponent *master = MasterOf(tree);
      if (!master)
         return BuildResponse_(500, "{\"error\":\"the stored object has no component\"}");

      AnsiString ownerAddress = Lower(Utf8_(owner->GetAddress()));
      AnsiString ownerName = OrganizerName(owner);
      AnsiString organizer = Imip::AddressOf(master->Value("ORGANIZER"));
      if (organizer.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"the event names no organiser to answer\"}");
      if (organizer == ownerAddress)
         return BuildResponse_(400, "{\"error\":\"the calendar's owner organises this event; answers are the guests'\"}");
      // An answer is a guest's: an event a phone copied in, a forwarded
      // invitation or one to a list the owner is not named on has no ATTENDEE
      // for the owner, and no organiser expecting their reply.
      if (MyPartstat(*master, ownerAddress).IsEmpty())
         return BuildResponse_(400, "{\"error\":\"the calendar's owner is not a guest of this event\"}");

      // The OWNER's answer is what is set and what is sent, whoever pressed
      // the button: a delegate's own calendar is not touched, and the
      // organiser is told who answered through SENT-BY on the ATTENDEE and
      // Sender: on the mail.
      bool onBehalf = !visible.mine;
      AnsiString delegateAddress = onBehalf ? Lower(Utf8_(account->GetAddress())) : AnsiString("");
      Imip::SetPartstat(tree, ownerAddress, ownerName, partstat, delegateAddress);

      // A room's or a piece of equipment's answer goes From it only to the
      // organiser it recorded when it accepted the booking, and only while
      // the booking still names them (ResourceBooking::AnswerableOrganiser):
      // anybody with a write share of its calendar can write any ORGANIZER
      // into the stored object. A booking it did not accept - one a delegate
      // wrote in by hand - is answered by the delegate themselves.
      //
      // A room or a piece of equipment holds what it agreed to and nothing it
      // refused: a booking its delegate declines leaves its calendar, as one
      // it declines itself does (ResourceBooking.h), since everything its
      // calendar holds that is not cancelled is busy. Whom to tell is read
      // under the room's lock, with the decline's delete, so that a decision
      // written in between is never read half way.
      AnsiString answerTo = organizer;
      const bool takenOut = owner->IsResource() && partstat == "DECLINED";
      if (owner->IsResource())
      {
         bool deleted = true;
         {
            ResourceCalendarGuard guard(visible.calendar.accountId);
            answerTo = ResourceBooking::AnswerableOrganiser(*owner, record.uid, organizer);
            if (takenOut)
            {
               __int64 token = 0;
               deleted = CalendarStore::Delete(visible.calendar.accountId, visible.calendar.id, record.id, token);
               if (deleted)
                  ResourceBooking::ForgetDecision(visible.calendar.accountId, record.uid);
            }
         }
         if (!deleted)
            return BuildResponse_(422, "{\"error\":\"the declined booking could not be taken out of the calendar\"}");
      }
      if (!takenOut && !Store(visible, tree, record, false, problem))
         return BuildResponse_(422, "{\"error\":\"the event could not be stored: " + JsonEscape_(problem) + "\"}");

      ImipMessage described;
      AnsiString ignored;
      Imip::Read(ICalendar::Serialize(tree), described, ignored);
      AnsiString replyText = Imip::ReplyObject(described, ownerAddress, ownerName, partstat, delegateAddress);
      AnsiString word = partstat == "ACCEPTED" ? "Accepted" : (partstat == "TENTATIVE" ? "Tentative" : "Declined");
      AnsiString subject = word + ": " + ICalendar::Unescape(described.summary) + " @ " + Imip::WhenText(described.start, described.end, described.allDay);
      AnsiString body = (ownerName.IsEmpty() ? ownerAddress : ownerName + " <" + ownerAddress + ">") + " has " + Lower(word) + (partstat == "TENTATIVE" ? "ly accepted" : "") + " this invitation";
      if (onBehalf)
      {
         AnsiString delegateName = OrganizerName(account);
         body += ", answered by " + (delegateName.IsEmpty() ? delegateAddress : delegateName + " <" + delegateAddress + ">") + " on their behalf";
      }
      body += ".\r\n\r\n" + Imip::Describe(described);
      AnsiString comment = Utf8_(JsonUtf8Value_(requestBody, "comment"));
      if (!comment.IsEmpty())
         body += "\r\n" + comment + "\r\n";

      // From the owner, Sender the delegate, and the delegate's own envelope:
      // what a mail client shows as "on behalf of".
      bool replied = false;
      if (answerTo.IsEmpty())
      {
         LOG_DEBUG("Calendar: the answer of " + owner->GetAddress() + " to " + record.uid +
            " was not mailed: it did not accept the booking from the organiser the booking names.");
      }
      else
      {
         AnsiString senderHeader = onBehalf ? Utf8_(FromHeader_(account)) : AnsiString("");
         AnsiString mail = Imip::BuildMail(Utf8_(FromHeader_(owner)), answerTo, subject, body, "REPLY", replyText, ownerAddress, senderHeader);
         String to;
         Unicode::MultiByteToWide(answerTo, to);
         AnsiString refusal;
         int status = QueueRawMessage_(account, to, mail, refusal);
         replied = status == 0;
         if (!replied)
            LOG_DEBUG("Calendar: the reply to " + String(answerTo) + " could not be queued: " + String(refusal));
      }

      LOG_DEBUG("Calendar: " + account->GetAddress() + " answered " + String(partstat) + " to " + record.uid + (onBehalf ? _T(" on behalf of ") + owner->GetAddress() : String()) + ".");
      return BuildResponse_(200, "{\"event_id\":" + Int64Text(record.id) + ",\"calendar\":" + Int64Text(visible.calendar.id) +
         ",\"partstat\":\"" + partstat + "\",\"on_behalf_of\":\"" + JsonEscape_(onBehalf ? ownerAddress : AnsiString("")) +
         "\",\"replied\":" + AnsiString(replied ? "true" : "false") + ",\"to\":\"" + JsonEscape_(answerTo) + "\"}");
   }

   AnsiString
   RestApiServer::OpenApiCalendarPaths_()
   {
      return
         ",\"/api/v1/me/calendars\":{\"get\":{\"summary\":\"The signed-in account's calendars, and the ones shared with it\",\"description\":\"Each with id, name (the CalDAV segment), display_name, color (#RRGGBB or empty), owner, mine, writable, is_default and, for the account's own, shares: the addresses it is shared with and whether they may write. The default calendar is made on first use.\",\"responses\":{\"200\":{\"description\":\"calendars\"}}},"
         "\"post\":{\"summary\":\"Make a calendar\",\"description\":\"Body: display_name (required), color (#RRGGBB), name (the URI segment; made from the display name when absent). A second calendar is a task list, a project, a shared diary: it is served over CalDAV under the home as /dav/calendars/{address}/{name}/.\",\"responses\":{\"201\":{\"description\":\"the calendar\"},\"400\":{\"description\":\"a field is wrong\"},\"409\":{\"description\":\"the name is taken\"}}}},"
         "\"/api/v1/me/calendars/{id}\":{\"get\":{\"summary\":\"One calendar the account can see, with its shares when it is the account's own\",\"description\":\"The same object as the listing gives; shares is a list of {address, write, delegate} for an account and {group, members, write, delegate} for a group of this server.\",\"responses\":{\"200\":{\"description\":\"the calendar\"},\"404\":{\"description\":\"no such calendar\"}}},"
         "\"put\":{\"summary\":\"Rename or recolour a calendar\",\"description\":\"Body: display_name and/or color. The owner's only.\",\"responses\":{\"200\":{\"description\":\"the calendar\"},\"403\":{\"description\":\"not the caller's calendar\"},\"404\":{\"description\":\"no such calendar\"}}},"
         "\"delete\":{\"summary\":\"Delete a calendar, or leave one shared with you\",\"description\":\"The account's own calendar goes with its events and shares; the default cannot be deleted. On a calendar another account shared, the share is given back and the calendar stays theirs.\",\"responses\":{\"200\":{\"description\":\"deleted, or left\"},\"403\":{\"description\":\"the default calendar\"},\"404\":{\"description\":\"no such calendar\"}}}},"
         "\"/api/v1/me/calendars/{id}/shares/{address}\":{\"put\":{\"summary\":\"Share a calendar with another account of this server, or with a group\",\"description\":\"The path names an account's address, or the name of one of this server's groups (a name that is both is the account). Body: write (boolean, default false), delegate (boolean, default false; implies write: the sharee may answer the owner's invitations on the owner's behalf through POST /api/v1/me/calendar/events/{id}/reply). The other account - or every member of the group, read at the time it asks - sees the calendar in its own listing and CalDAV home as shared-{id}, read-only or read-write; what it may do is what the CalDAV current-user-privilege-set says.\",\"responses\":{\"200\":{\"description\":\"the calendar with its shares\"},\"404\":{\"description\":\"no account has that address and no group that name\"}}},"
         "\"delete\":{\"summary\":\"Take a share back, from an account or from a group\",\"description\":\"One 404 whether the name is unknown or not shared with, so the route does not read the group list for anybody.\",\"responses\":{\"200\":{\"description\":\"the calendar with its shares\"},\"404\":{\"description\":\"not shared with that name\"}}}},"
         "\"/api/v1/me/calendar/events\":{\"get\":{\"summary\":\"The events of a window, occurrence by occurrence, across every calendar the account can see\",\"description\":\"start= and end= as UTC date-times (YYYYMMDDTHHMMSSZ), at most 400 days apart. Every recurring event is expanded by the server's recurrence module - overrides and exclusions applied - and each occurrence carries its recurrence_id, so the page can change or delete that one alone. Times are UTC; local_start and local_end are the wall clock in the event's own zone (tz) for the editor. my_partstat is the caller's answer when they are an attendee.\",\"responses\":{\"200\":{\"description\":\"events\"},\"400\":{\"description\":\"the window is missing or wrong\"}}},"
         "\"post\":{\"summary\":\"Create an event\",\"description\":\"Body: summary (required), start (required: YYYY-MM-DD for an all-day event, else YYYY-MM-DDTHH:MM wall time in tz), end (default an hour later, or the day), all_day, tz (an IANA zone this server knows; times are stored with that TZID so a weekly 09:00 stays 09:00 across a clock change), location, description, rrule (an RFC 5545 rule, e.g. FREQ=WEEKLY;BYDAY=MO;COUNT=10), alarm_minutes (a display reminder that many minutes before; -1 none), attendees (an array of addresses, Name <address> allowed), calendar (id; default the first), and invite: true to mail each attendee a METHOD:REQUEST with the event as text/calendar - what Outlook, Gmail and Apple Mail turn into an invitation card.\",\"responses\":{\"201\":{\"description\":\"id, uid, calendar, invited\"},\"400\":{\"description\":\"a field is wrong\"},\"403\":{\"description\":\"the calendar is read-only for the caller\"}}}},"
         "\"/api/v1/me/calendar/events/{id}\":{\"get\":{\"summary\":\"One event as stored, with its iCalendar text\",\"responses\":{\"200\":{\"description\":\"the event\"},\"404\":{\"description\":\"not found\"}}},"
         "\"put\":{\"summary\":\"Change an event, the series or one occurrence\",\"description\":\"The same fields as the create, each optional; what is left out keeps its value. With instance (an occurrence's recurrence_id) the change is written as an override of that occurrence alone - a moved meeting, a renamed one - and the rest of the series is untouched; without it the series changes. calendar moves the event to another calendar. invite: true mails the attendees again with the sequence stepped, which their clients read as an update.\",\"responses\":{\"200\":{\"description\":\"id, uid, calendar\"},\"400\":{\"description\":\"a field is wrong\"},\"403\":{\"description\":\"read-only\"},\"404\":{\"description\":\"not found, or no occurrence starts then\"}}},"
         "\"delete\":{\"summary\":\"Delete an event, or one occurrence of it\",\"description\":\"?instance= (a recurrence_id) excludes that occurrence from the series and drops its override; without it the event goes. ?notify=1 mails the attendees a METHOD:CANCEL first, when the caller organises the event.\",\"responses\":{\"200\":{\"description\":\"removed\"},\"404\":{\"description\":\"not found\"}}}},"
         "\"/api/v1/me/tasks\":{\"get\":{\"summary\":\"The tasks of every calendar the account can see\",\"description\":\"Every VTODO, soonest due first: summary, description, due (UTC) and local_due, all_day, completed and completed_at, priority (1 the highest, 0 none), status, the calendar and whether it is writable. What Apple Reminders and Tasks.org write over CalDAV is what is listed here, and the other way round.\",\"responses\":{\"200\":{\"description\":\"tasks\"}}},"
         "\"post\":{\"summary\":\"Add a task\",\"description\":\"Body: summary (required), due (YYYY-MM-DD, or YYYY-MM-DDTHH:MM with tz), description, priority (0-9), calendar (id; the first when absent).\",\"responses\":{\"201\":{\"description\":\"the task\"},\"400\":{\"description\":\"a field is wrong\"}}}},"
         "\"/api/v1/me/tasks/{id}\":{\"put\":{\"summary\":\"Change a task, or tick it off\",\"description\":\"Any of summary, description, due (empty to clear), tz, priority, calendar, and completed: true writes STATUS:COMPLETED, COMPLETED and PERCENT-COMPLETE:100 as a client does; false clears them.\",\"responses\":{\"200\":{\"description\":\"the task\"},\"404\":{\"description\":\"not found\"}}},"
         "\"delete\":{\"summary\":\"Delete a task\",\"responses\":{\"200\":{\"description\":\"removed\"},\"404\":{\"description\":\"not found\"}}}},"
         "\"/api/v1/me/messages/{id}/invitation\":{\"get\":{\"summary\":\"The invitation a message carries\",\"description\":\"The first text/calendar part or .ics attachment, read: method (REQUEST, REPLY, CANCEL, COUNTER, ...), the event's summary, when (UTC and local), where, organizer, attendees with their answers, my_partstat, and whether the event is already in one of the caller's calendars (in_calendar, event_id, stored_partstat, stored_status). 404 for a message without one.\",\"responses\":{\"200\":{\"description\":\"the invitation\"},\"404\":{\"description\":\"no invitation\"}}}},"
         "\"/api/v1/me/messages/{id}/invitation/reply\":{\"post\":{\"summary\":\"Answer an invitation\",\"description\":\"Body: action - accept, tentative or decline keep the event in the caller's default calendar with their PARTSTAT set (updating the copy a REQUEST delivered to the account already filed there, never adding a second) and mail the organiser a METHOD:REPLY (RFC 6047), which this server files into the organiser's own event when they are on it and which Exchange, Microsoft 365 and Google file into theirs; remove takes the event out of the calendar (what a CANCEL asks for); counter, with start and optionally end and tz, mails the organiser a METHOD:COUNTER proposing that time. comment travels in the mail.\",\"responses\":{\"200\":{\"description\":\"stored, replied, or countered\"},\"400\":{\"description\":\"the action is unknown, or the caller is the organiser\"},\"404\":{\"description\":\"no invitation\"}}}},"
         "\"/api/v1/me/calendar/events/{id}/reply\":{\"post\":{\"summary\":\"Answer an invitation that is in a calendar - as its owner, or as a delegate on the owner's behalf\",\"description\":\"Body: partstat - ACCEPTED, TENTATIVE or DECLINED - and an optional comment. Allowed to the calendar's owner and to a sharee whose share is a delegation (not to a plain writer or a reader: 403). The OWNER's PARTSTAT on the event is set and the organiser is mailed a METHOD:REPLY whose ATTENDEE is the owner; when a delegate answered, that ATTENDEE carries SENT-BY=mailto:the delegate (RFC 5545 section 3.2.18), kept in the stored event so the owner's page shows who answered, and the mail is From: the owner with Sender: the delegate, sent from the delegate's own address - what a mail client shows as on behalf of. The delegate's own calendar is not touched. The owner that is a room or a piece of equipment answers only the organiser it recorded when it accepted the booking itself, while the booking still names them: for a booking it did not accept, or whose ORGANIZER was changed since, the answer is stored (a decline takes the booking out of its calendar) and nothing is mailed - replied false, to empty. 400 when the event names no organiser or the owner organises it.\",\"responses\":{\"200\":{\"description\":\"event_id, partstat, on_behalf_of, replied, to\"},\"400\":{\"description\":\"partstat is not one of the three, the owner organises the event, or the owner is not among its guests\"},\"403\":{\"description\":\"not the owner and not a delegate\"},\"404\":{\"description\":\"no such event\"}}}},"
         "\"/api/v1/me/freebusy\":{\"get\":{\"summary\":\"When an account of the caller's domain is busy\",\"description\":\"address= an account of the caller's own domain (an alias domain's spelling resolved); from= and to= the window, at most 62 days apart, as YYYY-MM-DD, YYYY-MM-DDTHH:MM[:SS] with Z or an offset, or YYYYMMDDTHHMMSSZ. The answer is the busy periods of that account's own calendars over the window - every event instance expanded, recurrences included, an event that is TRANSP:TRANSPARENT or STATUS:CANCELLED or that the account declined left out - merged and sorted, each with kind busy or tentative (tentative when the account's PARTSTAT is TENTATIVE or NEEDS-ACTION), with no title, location or description. 404 for an address that is not an active account of the caller's domain, one that does not exist, and one whose freebusy preference is nobody: one answer, so nothing is enumerated. The CalDAV free-busy-query REPORT answers the same computation as a VFREEBUSY.\",\"responses\":{\"200\":{\"description\":\"address, from, to, busy: [{start, end, kind}]\"},\"400\":{\"description\":\"the address or the window is missing, or the window is over 62 days\"},\"404\":{\"description\":\"not an account of the caller's domain that shows when it is busy\"}}}}";
   }
}
