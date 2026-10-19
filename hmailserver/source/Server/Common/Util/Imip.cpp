// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See Imip.h.

#include "StdAfx.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <ctime>
#include <set>
#include <stdexcept>

#include "Imip.h"
#include "CalendarStore.h"
#include "DavSupport.h"
#include "File.h"
#include "ByteBuffer.h"
#include "FileUtilities.h"
#include "Unicode.h"
#include "Time.h"
#include "GUIDCreator.h"
#include "Encoding/Base64.h"
#include "Parsing/AddresslistParser.h"
#include "../BO/Account.h"
#include "../BO/Alias.h"
#include "../BO/DomainAliases.h"
#include "../BO/DistributionList.h"
#include "../BO/Message.h"
#include "../Application/ObjectCache.h"
#include "../Cache/CacheContainer.h"
#include "../Mime/Mime.h"
#include "../Mime/MimeCode.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../Application/Logger.h"
#include "../AntiSpam/DMARC/DMARC.h"
#include "../../SMTP/SMTPConnection.h"
#include "DistributionListMembership.h"
#include "ResourceBooking.h"

namespace HM
{
   namespace
   {
      // A message this size or larger is not looked into for a calendar part:
      // an invitation is a few kilobytes, and the filer runs on the delivery
      // thread.
      const __int64 MaxLookedAtBytes = 1024 * 1024;

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

      bool ContainsNoCase(const AnsiString &haystack, const char *needle)
      {
         return Lower(haystack).Find(needle) >= 0;
      }

      AnsiString Utf8Of(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      // The media type of a Content-Type value, lower-cased: "text/calendar"
      // from "text/calendar; charset=utf-8; method=REQUEST". The MIME reader
      // hands back the raw field value, parameters and all, so a message whose
      // whole body is the calendar - what a client sends when it does not add
      // a text part - was never matched by the bare type; the multipart case
      // matched by the part's .ics name, which hid it.
      AnsiString MediaTypeOf(const char *contentType)
      {
         AnsiString type = Lower(Trimmed(AnsiString(contentType ? contentType : "")));
         int semicolon = type.Find(";");
         if (semicolon >= 0)
            type = Trimmed(type.Mid(0, semicolon));
         return type;
      }

      // The first text/calendar part below this one, or an attachment named
      // *.ics, decoded from its transfer encoding.
      bool FindCalendarPart(std::shared_ptr<MimeBody> body, int depth, AnsiString &calendarText)
      {
         if (!body || depth > 8)
            return false;

         AnsiString type = MediaTypeOf(body->GetContentType());
         AnsiString name = Lower(AnsiString(body->GetName().c_str()));
         bool calendar = type == "text/calendar" || (name.GetLength() > 4 && name.EndsWith(".ics"));
         if (calendar && !body->IsMultiPart())
         {
            AnsiString raw(body->GetRawText().c_str());
            AnsiString decoded;
            MimeCodeBase *coder = MimeEnvironment::CreateCoder(body->GetTransferEncoding());
            if (coder)
            {
               coder->SetInput(raw.c_str(), raw.GetLength(), false);
               coder->GetOutput(decoded);
               delete coder;
            }
            else
               decoded = raw;
            calendarText = decoded;
            return true;
         }

         if (!body->IsMultiPart())
            return false;

         std::shared_ptr<MimeBody> part = body->FindFirstPart();
         while (part)
         {
            if (FindCalendarPart(part, depth + 1, calendarText))
               return true;
            part = body->FindNextPart();
         }
         return false;
      }

      const ICalComponent *MasterOf(const ICalComponent &calendar)
      {
         const ICalComponent *first = nullptr;
         for (size_t i = 0; i < calendar.children.size(); i++)
         {
            const ICalComponent &component = calendar.children[i];
            if (component.name != "VEVENT" && component.name != "VTODO")
               continue;
            if (!component.Property("RECURRENCE-ID"))
               return &component;
            if (!first)
               first = &component;
         }
         return first;
      }

      void ReadAttendee(const ICalProperty &property, ImipAttendee &attendee)
      {
         attendee.address = Imip::AddressOf(property.value);
         attendee.name = property.Parameter("CN");
         attendee.partstat = property.Parameter("PARTSTAT");
         if (attendee.partstat.IsEmpty())
            attendee.partstat = "NEEDS-ACTION";
         attendee.partstat.ToUpper();
         attendee.role = property.Parameter("ROLE");
         if (attendee.role.IsEmpty())
            attendee.role = "REQ-PARTICIPANT";
         attendee.role.ToUpper();
         attendee.sentBy = Imip::AddressOf(property.Parameter("SENT-BY"));
      }

      AnsiString MonthName(int month)
      {
         static const char *names[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
         return month >= 1 && month <= 12 ? AnsiString(names[month - 1]) : AnsiString("");
      }

      void Fields(__int64 instant, int &y, int &m, int &d, int &hh, int &mm)
      {
         int ss = 0;
         ICalendar::InstantToLocal("UTC", instant, y, m, d, hh, mm, ss);
      }

      AnsiString TwoDigits(int value)
      {
         AnsiString text;
         text.Format("%02d", value);
         return text;
      }

      AnsiString HeaderSafe(const AnsiString &text)
      {
         AnsiString out;
         for (int i = 0; i < text.GetLength(); i++)
         {
            unsigned char c = static_cast<unsigned char>(text.c_str()[i]);
            if (c == '\r' || c == '\n')
               continue;
            out += static_cast<char>(c);
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

      // RFC 2047 for a subject with anything beyond ASCII: base64, UTF-8.
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
   }

   AnsiString
   Imip::AddressOf(const AnsiString &calAddress)
   {
      AnsiString address = Trimmed(calAddress);
      if (Lower(address).StartsWith("mailto:"))
         address = address.Mid(7);
      return Lower(Trimmed(address));
   }

   bool
   Imip::FindInFile(const String &fileName, AnsiString &calendarText)
   {
      calendarText = "";

      __int64 size = MessageStore::FileSize(fileName);
      if (size <= 0 || size > MaxLookedAtBytes)
         return false;

      // The head first: a message that is neither multipart nor a calendar
      // itself has no part to find, whatever its size.
      AnsiString header = PersistentMessage::LoadHeader(fileName, false);
      MimeHeader mimeHeader;
      mimeHeader.Load(header.c_str(), header.GetLength(), true);
      AnsiString type = MediaTypeOf(mimeHeader.GetContentType());
      bool multipart = type.StartsWith("multipart/");
      if (!multipart && type != "text/calendar")
         return false;

      if (multipart)
      {
         // The bytes must mention the part before the whole message is parsed.
         AnsiString whole;
         if (!MessageStore::ReadAll(fileName, whole))
            return false;
         if (!ContainsNoCase(whole, "text/calendar") && !ContainsNoCase(whole, ".ics"))
            return false;
      }

      std::shared_ptr<MimeBody> body = std::shared_ptr<MimeBody>(new MimeBody);
      if (MessageStore::LoadMime(Utf8Of(fileName), *body) != MimeLoadResult::Loaded)
         return false;

      return FindCalendarPart(body, 0, calendarText) && !Trimmed(calendarText).IsEmpty();
   }

   bool
   Imip::Read(const AnsiString &calendarText, ImipMessage &out, AnsiString &problem)
   {
      out = ImipMessage();
      out.text = calendarText;
      if (!ICalendar::Parse(calendarText, out.tree, problem))
         return false;

      out.method = Trimmed(out.tree.Value("METHOD"));
      out.method.ToUpper();

      const ICalComponent *master = MasterOf(out.tree);
      if (!master)
      {
         problem = "the object has no VEVENT or VTODO";
         return false;
      }

      ICalendar::Summary summary;
      AnsiString summaryProblem;
      if (ICalendar::Summarize(out.tree, summary, summaryProblem))
      {
         out.start = summary.start;
         out.end = summary.end;
         out.recurring = summary.recurring;
      }
      else
      {
         // An object the store would refuse - several UIDs, a zone this server
         // cannot place - is still described from its master.
         const ICalProperty *dtstart = master->Property("DTSTART");
         ICalTime time;
         AnsiString ignored;
         if (dtstart && ICalendar::ParseTime(*dtstart, &out.tree, time, ignored))
            out.start = time.instant;
         const ICalProperty *dtend = master->Property(master->name == "VTODO" ? "DUE" : "DTEND");
         if (dtend && ICalendar::ParseTime(*dtend, &out.tree, time, ignored))
            out.end = time.instant;
         else
            out.end = out.start;
         out.recurring = master->Property("RRULE") != nullptr;
      }

      out.component = master->name;
      out.uid = Trimmed(master->Value("UID"));
      out.summary = master->Value("SUMMARY");
      out.location = master->Value("LOCATION");
      out.description = master->Value("DESCRIPTION");
      out.rrule = master->Value("RRULE");
      if (const ICalProperty *dtstart = master->Property("DTSTART"))
      {
         out.allDay = dtstart->HasParameter("VALUE", "DATE");
         out.tzid = dtstart->Parameter("TZID");
      }
      out.sequence = SequenceOf(*master);

      if (const ICalProperty *organizer = master->Property("ORGANIZER"))
      {
         out.organizer = AddressOf(organizer->value);
         out.organizerName = organizer->Parameter("CN");
      }

      for (size_t i = 0; i < master->properties.size(); i++)
      {
         if (master->properties[i].name != "ATTENDEE")
            continue;
         ImipAttendee attendee;
         ReadAttendee(master->properties[i], attendee);
         if (!attendee.address.IsEmpty())
            out.attendees.push_back(attendee);
      }

      return true;
   }

   AnsiString
   Imip::WithMethod(const ICalComponent &calendar, const AnsiString &method)
   {
      ICalComponent copy = calendar;
      bool set = false;
      if (method.IsEmpty())
      {
         // In one pass, rather than one at a time from the middle.
         copy.properties.erase(std::remove_if(copy.properties.begin(), copy.properties.end(), [](const ICalProperty &property)
            {
               return property.name == "METHOD";
            }), copy.properties.end());
      }

      for (size_t i = 0; i < copy.properties.size(); i++)
      {
         if (copy.properties[i].name == "METHOD")
         {
            copy.properties[i].value = method;
            set = true;
         }
      }
      if (!set && !method.IsEmpty())
      {
         ICalProperty property;
         property.name = "METHOD";
         property.value = method;
         // After VERSION and PRODID, where every reader looks for it.
         size_t at = 0;
         for (size_t i = 0; i < copy.properties.size(); i++)
         {
            if (copy.properties[i].name == "VERSION" || copy.properties[i].name == "PRODID")
               at = i + 1;
         }
         copy.properties.insert(copy.properties.begin() + at, property);
      }
      return ICalendar::Serialize(copy);
   }

   void
   Imip::SetPartstat(ICalComponent &calendar, const AnsiString &address, const AnsiString &name, const AnsiString &partstat, const AnsiString &sentBy,
                     bool addWhenAbsent)
   {
      AnsiString wanted = Lower(address);
      for (size_t c = 0; c < calendar.children.size(); c++)
      {
         ICalComponent &component = calendar.children[c];
         if (component.name != "VEVENT" && component.name != "VTODO")
            continue;

         bool found = false;
         for (size_t i = 0; i < component.properties.size(); i++)
         {
            ICalProperty &property = component.properties[i];
            if (property.name != "ATTENDEE" || AddressOf(property.value) != wanted)
               continue;
            found = true;
            bool had = false;
            std::vector<std::pair<AnsiString, AnsiString>> &parameters = property.parameters;
            size_t write = 0;
            for (size_t p = 0; p < parameters.size(); p++)
            {
               if (parameters[p].first == "PARTSTAT")
               {
                  parameters[p].second = partstat;
                  had = true;
               }
               else if (parameters[p].first == "RSVP")
                  parameters[p].second = "FALSE";
               else if (parameters[p].first == "SENT-BY")
                  continue;   // whoever answered last is who answered

               // The others kept in their order, in one pass.
               if (write != p)
                  parameters[write] = std::move(parameters[p]);
               write++;
            }
            parameters.erase(parameters.begin() + write, parameters.end());
            if (!had)
               property.parameters.push_back(std::make_pair(AnsiString("PARTSTAT"), partstat));
            if (!sentBy.IsEmpty())
               property.parameters.push_back(std::make_pair(AnsiString("SENT-BY"), "mailto:" + Lower(sentBy)));
         }

         if (!found && addWhenAbsent)
         {
            ICalProperty property;
            property.name = "ATTENDEE";
            if (!name.IsEmpty())
               property.parameters.push_back(std::make_pair(AnsiString("CN"), name));
            property.parameters.push_back(std::make_pair(AnsiString("PARTSTAT"), partstat));
            if (!sentBy.IsEmpty())
               property.parameters.push_back(std::make_pair(AnsiString("SENT-BY"), "mailto:" + Lower(sentBy)));
            property.value = "mailto:" + address;
            component.properties.push_back(property);
         }
      }
   }

   AnsiString
   Imip::ReplyObject(const ImipMessage &invitation, const AnsiString &address, const AnsiString &name, const AnsiString &partstat, const AnsiString &sentBy,
                     const AnsiString &cutype)
   {
      ICalComponent reply;
      reply.name = "VCALENDAR";
      ICalProperty version;
      version.name = "VERSION";
      version.value = "2.0";
      reply.properties.push_back(version);
      ICalProperty prodid;
      prodid.name = "PRODID";
      prodid.value = "-//hMailServer//webmail//EN";
      reply.properties.push_back(prodid);
      ICalProperty method;
      method.name = "METHOD";
      method.value = "REPLY";
      reply.properties.push_back(method);

      // The timezones the master refers to travel with the reply.
      for (size_t i = 0; i < invitation.tree.children.size(); i++)
      {
         if (invitation.tree.children[i].name == "VTIMEZONE")
            reply.children.push_back(invitation.tree.children[i]);
      }

      const ICalComponent *master = MasterOf(invitation.tree);
      ICalComponent component;
      component.name = master ? master->name : AnsiString("VEVENT");
      static const char *kept[] = { "UID", "DTSTART", "DTEND", "DUE", "DURATION", "SUMMARY", "ORGANIZER", "SEQUENCE", "RECURRENCE-ID", "DTSTAMP" };
      if (master)
      {
         for (size_t i = 0; i < master->properties.size(); i++)
         {
            const ICalProperty &property = master->properties[i];
            for (size_t k = 0; k < sizeof(kept) / sizeof(kept[0]); k++)
            {
               if (property.name == kept[k] && !(property.name == "DTSTAMP"))
                  component.properties.push_back(property);
            }
         }
      }

      ICalProperty stamp;
      stamp.name = "DTSTAMP";
      stamp.value = ICalendar::FormatUtc(static_cast<__int64>(time(nullptr)));
      component.properties.push_back(stamp);

      ICalProperty attendee;
      attendee.name = "ATTENDEE";
      if (!name.IsEmpty())
         attendee.parameters.push_back(std::make_pair(AnsiString("CN"), name));
      if (!cutype.IsEmpty())
         attendee.parameters.push_back(std::make_pair(AnsiString("CUTYPE"), cutype));
      attendee.parameters.push_back(std::make_pair(AnsiString("PARTSTAT"), partstat));
      if (!sentBy.IsEmpty())
         attendee.parameters.push_back(std::make_pair(AnsiString("SENT-BY"), "mailto:" + Lower(sentBy)));
      attendee.value = "mailto:" + address;
      component.properties.push_back(attendee);

      reply.children.push_back(component);
      return ICalendar::Serialize(reply);
   }

   AnsiString
   Imip::WhenText(__int64 start, __int64 end, bool allDay)
   {
      int y, m, d, hh, mm;
      Fields(start, y, m, d, hh, mm);
      AnsiString text;
      if (allDay)
      {
         text = MonthName(m) + " " + TwoDigits(d) + ", " + AnsiString(std::to_string(y).c_str());
         int y2, m2, d2, hh2, mm2;
         Fields(end - 1, y2, m2, d2, hh2, mm2);
         if (end > start + 86400)
            text += " - " + MonthName(m2) + " " + TwoDigits(d2) + ", " + AnsiString(std::to_string(y2).c_str());
         return text;
      }
      text = MonthName(m) + " " + TwoDigits(d) + ", " + AnsiString(std::to_string(y).c_str()) + " " + TwoDigits(hh) + ":" + TwoDigits(mm);
      if (end > start)
      {
         int y2, m2, d2, hh2, mm2;
         Fields(end, y2, m2, d2, hh2, mm2);
         if (y2 == y && m2 == m && d2 == d)
            text += " - " + TwoDigits(hh2) + ":" + TwoDigits(mm2);
         else
            text += " - " + MonthName(m2) + " " + TwoDigits(d2) + ", " + AnsiString(std::to_string(y2).c_str()) + " " + TwoDigits(hh2) + ":" + TwoDigits(mm2);
      }
      return text + " UTC";
   }

   AnsiString
   Imip::Describe(const ImipMessage &message)
   {
      AnsiString text = "When: " + WhenText(message.start, message.end, message.allDay) + "\r\n";
      if (!message.location.IsEmpty())
         text += "Where: " + ICalendar::Unescape(message.location) + "\r\n";
      if (!message.organizer.IsEmpty())
         text += "Organizer: " + (message.organizerName.IsEmpty() ? message.organizer : message.organizerName + " <" + message.organizer + ">") + "\r\n";
      if (!message.attendees.empty())
      {
         text += "Who: ";
         for (size_t i = 0; i < message.attendees.size(); i++)
         {
            if (i > 0)
               text += ", ";
            text += message.attendees[i].name.IsEmpty() ? message.attendees[i].address : message.attendees[i].name + " <" + message.attendees[i].address + ">";
         }
         text += "\r\n";
      }
      if (!message.description.IsEmpty())
         text += "\r\n" + ICalendar::Unescape(message.description) + "\r\n";
      return text;
   }

   AnsiString
   Imip::BuildMail(const AnsiString &fromHeader, const AnsiString &to, const AnsiString &subject,
                   const AnsiString &bodyText, const AnsiString &method, const AnsiString &calendarText, const AnsiString &fromAddress,
                   const AnsiString &senderHeader)
   {
      AnsiString domain = fromAddress.Find("@") >= 0 ? fromAddress.Mid(fromAddress.Find("@") + 1) : fromAddress;
      AnsiString id = "<" + Utf8Of(GUIDCreator::GetGUID()) + "@" + domain + ">";
      AnsiString boundary = "hm-imip-" + Utf8Of(GUIDCreator::GetGUID());
      const char *crlf = "\r\n";

      AnsiString text;
      text += "From: " + HeaderSafe(fromHeader) + crlf;
      if (!senderHeader.IsEmpty())
         text += "Sender: " + HeaderSafe(senderHeader) + crlf;
      text += "To: <" + HeaderSafe(to) + ">" + crlf;
      text += "Subject: " + EncodedWord(HeaderSafe(subject)) + crlf;
      text += "Date: " + Utf8Of(Time::GetCurrentMimeDate()) + crlf;
      text += "Message-ID: " + id + crlf;
      text += "MIME-Version: 1.0" + AnsiString(crlf);
      text += "X-hMailServer-iMIP: " + method + crlf;
      text += "Content-Type: multipart/alternative; boundary=\"" + boundary + "\"" + crlf + crlf;
      text += "--" + boundary + crlf;
      text += "Content-Type: text/plain; charset=utf-8" + AnsiString(crlf);
      text += "Content-Transfer-Encoding: base64" + AnsiString(crlf) + crlf;
      text += Base64Lines(bodyText);
      text += "--" + boundary + crlf;
      text += "Content-Type: text/calendar; charset=utf-8; method=" + method + "; name=\"invite.ics\"" + crlf;
      text += "Content-Transfer-Encoding: base64" + AnsiString(crlf);
      text += "Content-Disposition: inline; filename=\"invite.ics\"" + AnsiString(crlf) + crlf;
      text += Base64Lines(calendarText);
      text += "--" + boundary + "--" + crlf;
      return text;
   }

   String
   Imip::UriFor(const AnsiString &uid)
   {
      AnsiString safe;
      for (int i = 0; i < uid.GetLength() && safe.GetLength() < 200; i++)
      {
         char c = uid.c_str()[i];
         bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '@' || c == '.' || c == '_' || c == '-';
         safe += ok ? c : '_';
      }
      if (safe.IsEmpty())
         safe = Utf8Of(GUIDCreator::GetGUID());
      String uri;
      Unicode::MultiByteToWide(safe + ".ics", uri);
      return uri;
   }

   namespace
   {
      // The object written to the store as the page's routes write one: parsed
      // again, summarised, stamped. record carries the row on the way in (its
      // id and uri, for an update) and the row as written on the way out.
      bool WriteObject(__int64 accountId, __int64 calendarId, const ICalComponent &tree, CalendarObjectRecord &record, bool insert, AnsiString &problem)
      {
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
            if (!CalendarStore::Insert(accountId, calendarId, record, inserted, token))
            {
               problem = "the object could not be stored";
               return false;
            }
            record = inserted;
            return true;
         }
         if (!CalendarStore::Update(accountId, calendarId, record.id, record, token))
         {
            problem = "the object could not be saved";
            return false;
         }
         return true;
      }

      // The address's PARTSTAT on the component, upper-cased, and the SENT-BY
      // beside it; both "" when the address is not among its attendees.
      void ReadPartstat(const ICalComponent &component, const AnsiString &address, AnsiString &partstat, AnsiString &sentBy)
      {
         partstat = "";
         sentBy = "";
         AnsiString wanted = Lower(address);
         for (size_t i = 0; i < component.properties.size(); i++)
         {
            const ICalProperty &property = component.properties[i];
            if (property.name != "ATTENDEE" || Imip::AddressOf(property.value) != wanted)
               continue;
            partstat = Trimmed(property.Parameter("PARTSTAT"));
            partstat.ToUpper();
            sentBy = Imip::AddressOf(property.Parameter("SENT-BY"));
            return;
         }
      }

      void MarkCancelled(ICalComponent &tree)
      {
         for (size_t i = 0; i < tree.children.size(); i++)
         {
            ICalComponent &component = tree.children[i];
            if (component.name != "VEVENT" && component.name != "VTODO")
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

      // ---- the occurrences a CANCEL names (Imip::OccurrencesHeld) ----------

      // One occurrence a CANCEL names: the instant its RECURRENCE-ID places,
      // and whether every occurrence after it is named too.
      struct NamedOccurrence
      {
         __int64 instant;
         bool andAfter;
      };

      bool IsEventLike(const ICalComponent &component)
      {
         return component.name == "VEVENT" || component.name == "VTODO";
      }

      // The occurrences cancel's events name, each placed in cancel's own
      // zones; one whose RECURRENCE-ID cannot be placed names nothing.
      std::vector<NamedOccurrence> NamedOccurrences(const ICalComponent &cancel)
      {
         std::vector<NamedOccurrence> named;
         ICalTimeReader times(&cancel);   // each zone built once, not once an event
         for (size_t i = 0; i < cancel.children.size(); i++)
         {
            const ICalComponent &component = cancel.children[i];
            if (!IsEventLike(component))
               continue;
            const ICalProperty *recurrenceId = component.Property("RECURRENCE-ID");
            if (!recurrenceId)
               continue;
            ICalTime time;
            AnsiString ignored;
            if (!times.Read(*recurrenceId, time, ignored))
               continue;
            AnsiString range = Trimmed(recurrenceId->Parameter("RANGE"));
            range.ToUpper();
            NamedOccurrence occurrence = {};
            occurrence.instant = time.instant;
            occurrence.andAfter = range == "THISANDFUTURE";
            named.push_back(occurrence);
         }
         return named;
      }

      // The instant a stored component's RECURRENCE-ID names, read in the
      // stored object's zones (times: a reader over the stored object, one for
      // all the components a loop asks about).
      bool RecurrenceIdOf(const ICalComponent &component, ICalTimeReader &times, __int64 &instant)
      {
         const ICalProperty *recurrenceId = component.Property("RECURRENCE-ID");
         if (!recurrenceId)
            return false;
         ICalTime time;
         AnsiString ignored;
         if (!times.Read(*recurrenceId, time, ignored))
            return false;
         instant = time.instant;
         return true;
      }

      // The component of the object without a RECURRENCE-ID - its series -
      // or null when it holds overrides alone.
      const ICalComponent *SeriesMasterOf(const ICalComponent &tree)
      {
         for (size_t i = 0; i < tree.children.size(); i++)
         {
            if (IsEventLike(tree.children[i]) && !tree.children[i].Property("RECURRENCE-ID"))
               return &tree.children[i];
         }
         return nullptr;
      }

      ICalComponent *MutableSeriesMasterOf(ICalComponent &tree)
      {
         for (size_t i = 0; i < tree.children.size(); i++)
         {
            if (IsEventLike(tree.children[i]) && !tree.children[i].Property("RECURRENCE-ID"))
               return &tree.children[i];
         }
         return nullptr;
      }

      // The object with its master alone - its zones kept, its overrides
      // gone - for asking what the series itself makes.
      ICalComponent SeriesOnly(const ICalComponent &tree)
      {
         ICalComponent series = tree;
         series.children.clear();
         for (size_t i = 0; i < tree.children.size(); i++)
         {
            const ICalComponent &component = tree.children[i];
            if (!IsEventLike(component) || !component.Property("RECURRENCE-ID"))
               series.children.push_back(component);
         }
         return series;
      }

      // Whether a named occurrence covers an instant.
      bool Covers(const std::vector<NamedOccurrence> &named, __int64 instant)
      {
         for (size_t i = 0; i < named.size(); i++)
         {
            if (named[i].instant == instant || (named[i].andAfter && instant >= named[i].instant))
               return true;
         }
         return false;
      }

      // The occurrences the series of tree makes - not its overrides - from
      // windowStart to windowEnd, at most maxInstances, by the start the
      // recurrence gives each. False when the object cannot be expanded.
      // cutShort, when given, says whether the expansion stopped at
      // maxInstances before the end of the window.
      bool SeriesInstances(const ICalComponent &tree, __int64 windowStart, __int64 windowEnd, size_t maxInstances, std::vector<__int64> &recurrenceIds,
                           bool *cutShort = nullptr)
      {
         recurrenceIds.clear();
         if (cutShort)
            *cutShort = false;
         if (!SeriesMasterOf(tree))
            return true;
         std::vector<ICalInstance> instances;
         bool truncated = false;
         AnsiString problem;
         if (!ICalendar::Expand(SeriesOnly(tree), windowStart, windowEnd, maxInstances, instances, truncated, problem))
            return false;
         if (cutShort)
            *cutShort = truncated;
         for (size_t i = 0; i < instances.size(); i++)
            recurrenceIds.push_back(instances[i].recurrenceId);
         return true;
      }

      // The earliest start of anything the object holds, a little before it,
      // as the window an expansion from the beginning starts at.
      __int64 BeginningOf(const ICalComponent &tree)
      {
         ICalendar::Summary summary;
         AnsiString problem;
         if (!ICalendar::Summarize(tree, summary, problem))
            return 0;
         return summary.first - 1;
      }

      // The most instances MadeOfNamed asks one expansion for (the most
      // ICalendar::Expand gives), and the most expansions it makes.
      const size_t MaxSpanInstances = 10000;
      const size_t MaxSpanWindows = 16;

      // Which of the occurrences named alone (not with every one after it)
      // the series of tree makes: one of its instances, after its EXDATEs,
      // starts there. Every expansion walks the rule from its DTSTART, and a
      // CANCEL may name as many occurrences as a calendar object holds
      // components, so they are not asked one expansion each - which cost two
      // expansions an occurrence, on a delivery thread and on a REST worker
      // reading the card - but by one over the span from the first to the
      // last. A span holding more than MaxSpanInstances instances is read on
      // from the first named occurrence that expansion did not find, at most
      // MaxSpanWindows expansions in all; one still not found then is taken as
      // not made, which changes nothing for it.
      std::set<__int64> MadeOfNamed(const ICalComponent &tree, const std::vector<NamedOccurrence> &named)
      {
         std::set<__int64> wanted;
         for (size_t i = 0; i < named.size(); i++)
         {
            if (!named[i].andAfter)
               wanted.insert(named[i].instant);
         }

         std::set<__int64> made;
         std::set<__int64>::const_iterator next = wanted.begin();
         for (size_t window = 0; next != wanted.end() && window < MaxSpanWindows; window++)
         {
            std::vector<__int64> instances;
            bool cutShort = false;
            if (!SeriesInstances(tree, *next - 1, *wanted.rbegin() + 1, MaxSpanInstances, instances, &cutShort))
               break;
            for (size_t i = 0; i < instances.size(); i++)
            {
               if (wanted.count(instances[i]) > 0)
                  made.insert(instances[i]);
            }
            if (!cutShort)
               break;

            // On from the first one after this window's start that it did not
            // find: past where the expansion stopped, or one the series does
            // not make, which the next window passes over in turn.
            const __int64 from = *next;
            next = wanted.upper_bound(from);
            while (next != wanted.end() && made.count(*next) > 0)
               ++next;
         }
         return made;
      }

      // Whether the series of tree makes, from windowStart to windowEnd, an
      // occurrence that test takes, asked of its first ones: the first ask,
      // and when that expansion stops short with none taken, the most one
      // expansion gives. An expansion stops at a number of the rule's own
      // occurrences before its EXDATEs take any out (ICalendar::Expand), so a
      // series whose next occurrences are out already - a guest's copy that
      // took earlier CANCELs - answers none at first and more when asked
      // again. Two expansions at most, not more growing ones: each reads
      // every EXDATE the object holds, which an object of an iMIP message's
      // size may hold tens of thousands of. A series that cannot be
      // expanded, or that still stops short at the most, answers whenUnknown.
      bool SeriesMakesOne(const ICalComponent &tree, __int64 windowStart, __int64 windowEnd, size_t ask,
                          const std::function<bool(__int64 recurrenceId)> &test, bool whenUnknown)
      {
         ask = std::min<size_t>(std::max<size_t>(ask, 1), ICalendar::MaxInstances);
         while (true)
         {
            std::vector<__int64> made;
            bool cutShort = false;
            if (!SeriesInstances(tree, windowStart, windowEnd, ask, made, &cutShort))
               return whenUnknown;
            for (size_t i = 0; i < made.size(); i++)
            {
               if (test(made[i]))
                  return true;
            }
            if (!cutShort)
               return false;
            if (ask >= ICalendar::MaxInstances)
               return whenUnknown;
            ask = ICalendar::MaxInstances;
         }
      }

      // Whether the series of tree makes an occurrence at from or after it,
      // however many from there on are out already.
      bool SeriesMakesFrom(const ICalComponent &tree, __int64 from)
      {
         return SeriesMakesOne(tree, from, ICalendar::Forever, 64, [from](__int64 made) { return made >= from; }, false);
      }

      // How the master's DTSTART is written, which an EXDATE and an UNTIL
      // written into it follow: a day, floating, or an instant.
      enum class StartForm { Instant, Floating, Day };

      StartForm StartFormOf(const ICalComponent &master, const ICalComponent &tree)
      {
         const ICalProperty *dtstart = master.Property("DTSTART");
         if (!dtstart)
            return StartForm::Instant;
         ICalTime time;
         AnsiString ignored;
         if (!ICalendar::ParseTime(*dtstart, &tree, time, ignored))
            return StartForm::Instant;
         if (time.isDate)
            return StartForm::Day;
         return time.floating ? StartForm::Floating : StartForm::Instant;
      }

      // YYYYMMDD of the UTC day an instant falls on.
      AnsiString DayText(__int64 instant)
      {
         int y = 0, m = 0, d = 0, hh = 0, mm = 0, ss = 0;
         ICalendar::InstantToLocal("UTC", instant, y, m, d, hh, mm, ss);
         AnsiString text;
         text.Format("%04d%02d%02d", y, m, d);
         return text;
      }

      // An instant written in a form: the day it falls on, as a floating
      // time (the server reads a floating time as UTC, so the same digits
      // name the same instant), or in UTC.
      AnsiString TimeText(__int64 instant, StartForm form)
      {
         if (form == StartForm::Day)
            return DayText(instant);
         AnsiString utc = ICalendar::FormatUtc(instant);
         if (form == StartForm::Floating && utc.EndsWith("Z"))
            utc = utc.Mid(0, utc.GetLength() - 1);
         return utc;
      }

      // The occurrence at instant left out of the series: an EXDATE in the
      // form of the master's DTSTART (form: StartFormOf the master).
      void AddExdate(ICalComponent &master, StartForm form, __int64 instant)
      {
         ICalProperty exdate;
         exdate.name = "EXDATE";
         if (form == StartForm::Day)
            exdate.parameters.push_back(std::make_pair(AnsiString("VALUE"), AnsiString("DATE")));
         exdate.value = TimeText(instant, form);
         master.properties.push_back(exdate);
      }

      void AddExdate(ICalComponent &master, const ICalComponent &tree, __int64 instant)
      {
         AddExdate(master, StartFormOf(master, tree), instant);
      }

      // Every override whose RECURRENCE-ID a test takes, dropped.
      void DropOverrides(ICalComponent &tree, const std::function<bool(__int64 instant)> &drop)
      {
         // Decided for each first, then the others kept in their order in one
         // pass: erasing each from the middle moved every component after it,
         // a square in a stored event of many overrides, which a stranger's
         // invitation may be. What an override's RECURRENCE-ID reads as does
         // not depend on the other overrides, only on the time zones.
         std::vector<bool> dropped(tree.children.size(), false);
         bool any = false;
         ICalTimeReader times(&tree);
         for (size_t i = 0; i < tree.children.size(); i++)
         {
            __int64 instant = 0;
            if (IsEventLike(tree.children[i]) && RecurrenceIdOf(tree.children[i], times, instant) && drop(instant))
            {
               dropped[i] = true;
               any = true;
            }
         }

         if (!any)
            return;

         size_t write = 0;
         for (size_t i = 0; i < tree.children.size(); i++)
         {
            if (dropped[i])
               continue;
            if (write != i)
               tree.children[write] = std::move(tree.children[i]);
            write++;
         }
         tree.children.erase(tree.children.begin() + write, tree.children.end());
      }

      // The RDATE values of the master at from or after it, dropped, and an
      // RDATE left with none dropped with them. A PERIOD is placed by its
      // start; a value that cannot be placed is kept.
      //
      // Each value is read where it stands and the property's other parts
      // copied once: the list used to be cut a value at a time off its front,
      // and the whole property - its list with it - copied for every value, so
      // an RDATE of N values cost N copies of itself, and an RDATE emptied was
      // erased from the middle of the properties. The stored event can be a
      // stranger's invitation (a new one is filed from anybody), its list as
      // long as a message holds. The values' time zone is built once for them
      // all (ICalTimeReader), not once a value.
      void DropRdatesFrom(ICalComponent &master, const ICalComponent &tree, __int64 from)
      {
         std::vector<ICalProperty> &properties = master.properties;
         ICalTimeReader times(&tree);

         size_t write = 0;
         for (size_t p = 0; p < properties.size(); p++)
         {
            ICalProperty &property = properties[p];
            if (property.name == "RDATE")
            {
               AnsiString all;
               all.swap(property.value);

               const ICalProperty one = property;   // its name and parameters; each value is read in place of its own

               AnsiString kept;
               int position = 0;
               while (true)
               {
                  const int comma = all.Find(",", static_cast<size_t>(position));
                  const AnsiString value = comma >= 0 ? all.Mid(position, comma - position) : all.Mid(position);
                  const int slash = value.Find("/");
                  ICalTime time;
                  AnsiString ignored;
                  const bool placed = times.Read(one, Trimmed(slash > 0 ? value.Mid(0, slash) : value), time, ignored);
                  if (!placed || time.instant < from)
                  {
                     if (!kept.IsEmpty())
                        kept += ",";
                     kept += value;
                  }
                  if (comma < 0)
                     break;
                  position = comma + 1;
               }

               if (kept.IsEmpty())
                  continue;   // the RDATE goes

               property.value = kept;
            }

            if (write != p)
               properties[write] = std::move(properties[p]);
            write++;
         }

         properties.erase(properties.begin() + write, properties.end());
      }

      // The master's RRULE ended before from: UNTIL the moment before it, in
      // the form RFC 5545 asks of the DTSTART (a day for a series of days,
      // floating for a floating one, else UTC), in place of any COUNT or
      // UNTIL the rule had - only where the rule went on past from, since
      // an UNTIL in place of a COUNT that ended before it would add
      // occurrences.
      void EndRuleBefore(ICalComponent &master, const ICalComponent &tree, __int64 from)
      {
         ICalProperty *rrule = nullptr;
         for (size_t p = 0; p < master.properties.size() && !rrule; p++)
         {
            if (master.properties[p].name == "RRULE")
               rrule = &master.properties[p];
         }
         if (!rrule)
            return;

         // What the rule makes on its own: the series without its RDATEs and
         // EXDATEs.
         ICalComponent ruleOnly = SeriesOnly(tree);
         if (ICalComponent *ruleMaster = MutableSeriesMasterOf(ruleOnly))
         {
            // In one pass: erased one by one from the middle, a stranger's
            // event of many RDATE or EXDATE lines cost their square.
            std::vector<ICalProperty> &properties = ruleMaster->properties;
            properties.erase(std::remove_if(properties.begin(), properties.end(), [](const ICalProperty &property)
               {
                  return property.name == "RDATE" || property.name == "EXDATE";
               }), properties.end());
         }
         if (!SeriesMakesFrom(ruleOnly, from))
            return;

         // Each part read where it stands, not cut off the front of the rest.
         AnsiString rule;
         const AnsiString all = rrule->value;
         int position = 0;
         while (true)
         {
            const int semicolon = all.Find(";", static_cast<size_t>(position));
            const AnsiString part = Trimmed(semicolon >= 0 ? all.Mid(position, semicolon - position) : all.Mid(position));
            AnsiString key = part;
            const int equals = key.Find("=");
            if (equals >= 0)
               key = key.Mid(0, equals);
            key.ToUpper();
            if (!part.IsEmpty() && key != "COUNT" && key != "UNTIL")
            {
               if (!rule.IsEmpty())
                  rule += ";";
               rule += part;
            }
            if (semicolon < 0)
               break;
            position = semicolon + 1;
         }
         rule += ";UNTIL=" + TimeText(from - 1, StartFormOf(master, tree));
         rrule->value = rule;
      }

      // What tree holds of the named occurrences, and of others. madeOfNamed,
      // when given, is set to the occurrences named alone that the series
      // makes (MadeOfNamed), which TakeOutOccurrences then takes out without
      // asking again.
      ImipOccurrences Held(const ICalComponent &tree, const std::vector<NamedOccurrence> &named, std::set<__int64> *madeOfNamed = nullptr)
      {
         if (named.empty())
            return ImipOccurrences::NoneHeld;

         bool namedHeld = false;
         bool otherHeld = false;

         // The overrides.
         ICalTimeReader overrideTimes(&tree);
         for (size_t i = 0; i < tree.children.size(); i++)
         {
            __int64 instant = 0;
            if (!IsEventLike(tree.children[i]) || !RecurrenceIdOf(tree.children[i], overrideTimes, instant))
               continue;
            if (Covers(named, instant))
               namedHeld = true;
            else
               otherHeld = true;
         }

         // The series: each occurrence named is held when the series makes
         // it (MadeOfNamed; one named with every one after it, when the series
         // makes one from the earliest such on), and another is held when the
         // series makes any occurrence that none of them names - which an
         // expansion from the beginning to the first occurrence named with
         // every one after it finds within a few more instances than are
         // named, asked again for more while those it gave are out already
         // (SeriesMakesOne). A few expansions, whatever the number named. A
         // series that cannot be read so far is taken to hold others, so that
         // no more than the occurrences named is taken out of it: never the
         // whole series for a CANCEL of its first occurrence because the next
         // ones are out already.
         if (SeriesMasterOf(tree))
         {
            __int64 from = ICalendar::Forever;
            for (size_t i = 0; i < named.size(); i++)
            {
               if (named[i].andAfter)
                  from = std::min(from, named[i].instant);
            }
            const std::set<__int64> made = MadeOfNamed(tree, named);
            if (madeOfNamed)
               *madeOfNamed = made;
            if (!made.empty() || (from != ICalendar::Forever && SeriesMakesFrom(tree, from)))
               namedHeld = true;

            if (!otherHeld)
            {
               otherHeld = SeriesMakesOne(tree, BeginningOf(tree), from, named.size() + 2,
                  [&named](__int64 made) { return !Covers(named, made); }, true);
            }
         }

         if (!namedHeld)
            return ImipOccurrences::NoneHeld;
         return otherHeld ? ImipOccurrences::Some : ImipOccurrences::All;
      }

      // Why KeepInvitation refused an invitation whose organiser the server
      // could not verify sent it, when a copy of its UID was already stored.
      const char *NotReplacedUnverified = "a copy of the event is stored, and the server could not verify that the invitation's organiser sent it";

      String WideOf(const AnsiString &value)
      {
         String out;
         Unicode::MultiByteToWide(value, out);
         return out;
      }

      // The domain of an address, or "".
      AnsiString DomainPart(const AnsiString &address)
      {
         const int at = address.ReverseFind('@');
         return at > 0 ? address.Mid(at + 1) : AnsiString();
      }

      // Whether the address is at one of this server's own domains, an alias
      // domain resolved first.
      bool IsLocalAddress(const AnsiString &address)
      {
         String resolved = WideOf(address);
         std::shared_ptr<DomainAliases> domainAliases = ObjectCache::Instance()->GetDomainAliases();
         if (domainAliases)
            resolved = domainAliases->ApplyAliasesOnAddress(resolved);
         const AnsiString domain = DomainPart(Lower(Utf8Of(resolved)));
         return !domain.IsEmpty() && CacheContainer::Instance()->GetDomain(WideOf(domain)) != nullptr;
      }

      // The account of this server an address is, directly or through an
      // alias domain and a few hops of account aliases; none when it is not
      // one.
      std::shared_ptr<const Account> AccountOf(const AnsiString &address)
      {
         std::shared_ptr<DomainAliases> domainAliases = ObjectCache::Instance()->GetDomainAliases();
         String resolved = WideOf(address);
         if (domainAliases)
            resolved = domainAliases->ApplyAliasesOnAddress(resolved);
         for (int hop = 0; hop < 5; hop++)
         {
            std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(resolved);
            if (account)
               return account;
            std::shared_ptr<const Alias> alias = CacheContainer::Instance()->GetAlias(resolved);
            if (!alias || !alias->GetIsActive())
               break;
            resolved = alias->GetValue();
            if (domainAliases)
               resolved = domainAliases->ApplyAliasesOnAddress(resolved);
         }
         return std::shared_ptr<const Account>();
      }

      // Whether the account is a delegate the attendee named: the attendee is
      // another account of this server, one of whose calendars is shared with
      // the account as a delegate - by name or through a group - which is
      // what lets the account answer the attendee's invitations
      // (RestApiCalendar's reply route, a room's delegates).
      bool IsDelegateOf(const AnsiString &attendee, __int64 delegateAccountId)
      {
         if (delegateAccountId <= 0)
            return false;
         std::shared_ptr<const Account> owner = AccountOf(attendee);
         if (!owner || owner->GetID() == delegateAccountId)
            return false;
         std::vector<CalendarRecord> calendars;
         if (!CalendarStore::ListCalendarsIfAny(owner->GetID(), calendars))
            return false;
         for (size_t c = 0; c < calendars.size(); c++)
         {
            CalendarShareRecord share;
            if (CalendarStore::ShareOf(calendars[c].id, delegateAccountId, share) && share.delegate)
               return true;
         }
         return false;
      }

      // Whether a From field's value is written as one mailbox is, and as
      // nothing else: what DMARC vouches for is the address the sender's own
      // mail system let into the From, by its own reading of the field, so a
      // field another reader could take for a different address names nobody.
      // With its quoted strings and comments taken out (a backslash quoting
      // the character after it in either), what is left is a bare address, or
      // a display name and one <address> that ends the field: one '<', one
      // '>' after it and nothing but space after that, no '@' in the display
      // name and no ',', ';' or ':' outside the brackets (a list, a group). A
      // comment holds no '<', '>' or '@', and a display name, quoted or not,
      // no '<' or '>', and an '@' only when it is the address itself - as
      // Outlook writes a name it does not know, in single quotes or none.
      // address is the one both readers found.
      bool OneMailboxSyntax(const AnsiString &value, const AnsiString &address)
      {
         // A display name that holds an address holds this one.
         auto isTheAddress = [&address](const AnsiString &text) -> bool
         {
            AnsiString name = Lower(Trimmed(text));
            if (name.GetLength() >= 2 && name.c_str()[0] == '\'' && name.c_str()[name.GetLength() - 1] == '\'')
               name = Trimmed(name.Mid(1, name.GetLength() - 2));
            return name == address;
         };

         AnsiString bare;
         AnsiString quotedText;
         int depth = 0;
         bool quoted = false;
         for (int i = 0; i < value.GetLength(); i++)
         {
            const char c = value.c_str()[i];
            if (quoted || depth > 0)
            {
               if (c == '\\')
               {
                  // A quoted-pair: the next character is text, whatever it is.
                  if (i + 1 >= value.GetLength())
                     return false;
                  const char next = value.c_str()[++i];
                  if (next == '<' || next == '>' || (depth > 0 && next == '@'))
                     return false;
                  if (quoted)
                     quotedText += next;
                  continue;
               }
               if (quoted)
               {
                  if (c == '"')
                  {
                     quoted = false;
                     if (quotedText.Find("@") >= 0 && !isTheAddress(quotedText))
                        return false;
                     continue;
                  }
                  if (c == '<' || c == '>')
                     return false;
                  quotedText += c;
                  continue;
               }
               if (c == '(')
                  depth++;
               else if (c == ')')
                  depth--;
               else if (c == '<' || c == '>' || c == '@')
                  return false;
               continue;
            }
            if (c == '"')
            {
               quoted = true;
               quotedText = "";
               bare += ' ';
               continue;
            }
            if (c == '(')
            {
               depth = 1;
               bare += ' ';
               continue;
            }
            if (c == ')')
               return false;
            bare += c;
         }
         if (quoted || depth > 0)
            return false;

         int opens = 0;
         int closes = 0;
         int open = -1;
         int close = -1;
         for (int i = 0; i < bare.GetLength(); i++)
         {
            const char c = bare.c_str()[i];
            if (c == '<')
            {
               opens++;
               open = i;
            }
            else if (c == '>')
            {
               closes++;
               close = i;
            }
         }
         if (opens > 1 || closes != opens)
            return false;

         // What stands outside the brackets - the whole field, for a bare
         // address - is a name and space, never a list, a group or a route.
         AnsiString outside = opens == 1 ? bare.Mid(0, open) : bare;
         if (opens == 1)
         {
            if (close < open)
               return false;
            if (!Trimmed(bare.Mid(close + 1)).IsEmpty())
               return false;
            if (outside.Find("@") >= 0 && !isTheAddress(outside))
               return false;
         }
         else
         {
            const AnsiString one = Trimmed(bare);
            for (int i = 0; i < one.GetLength(); i++)
            {
               const char c = one.c_str()[i];
               if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                  return false;
            }
         }
         for (int i = 0; i < outside.GetLength(); i++)
         {
            const char c = outside.c_str()[i];
            if (c == ',' || c == ';' || c == ':')
               return false;
         }
         return true;
      }

      // The server's own mark on a stored copy filed from an invitation whose
      // organiser it did not verify (Imip::FiledUnverified).
      const char *UnverifiedMark = "X-HMAILSERVER-IMIP-UNVERIFIED";

      // Every VEVENT and VTODO of the object without the mark, or with it on
      // the master alone.
      void SetUnverifiedMark(ICalComponent &tree, bool marked)
      {
         for (size_t c = 0; c < tree.children.size(); c++)
         {
            ICalComponent &component = tree.children[c];
            if (component.name != "VEVENT" && component.name != "VTODO")
               continue;
            // In one pass, the others kept in their order.
            component.properties.erase(std::remove_if(component.properties.begin(), component.properties.end(), [](const ICalProperty &property)
               {
                  return property.name == UnverifiedMark;
               }), component.properties.end());
         }
         if (!marked)
            return;
         ICalComponent *master = nullptr;
         for (size_t c = 0; c < tree.children.size() && !master; c++)
         {
            ICalComponent &component = tree.children[c];
            if ((component.name == "VEVENT" || component.name == "VTODO") && !component.Property("RECURRENCE-ID"))
               master = &component;
         }
         for (size_t c = 0; c < tree.children.size() && !master; c++)
         {
            ICalComponent &component = tree.children[c];
            if (component.name == "VEVENT" || component.name == "VTODO")
               master = &component;
         }
         if (!master)
            return;
         ICalProperty mark;
         mark.name = UnverifiedMark;
         mark.value = "TRUE";
         master->properties.push_back(mark);
      }

      // The address added beside a distribution list the event lists, on the
      // components that list the list and not the address: MEMBER the list,
      // and the answer, the name and the SENT-BY given.
      bool AddListMember(ICalComponent &tree, const AnsiString &list, const AnsiString &address, const AnsiString &name, const AnsiString &partstat,
                         const AnsiString &sentBy)
      {
         bool added = false;
         for (size_t c = 0; c < tree.children.size(); c++)
         {
            ICalComponent &component = tree.children[c];
            if (component.name != "VEVENT" && component.name != "VTODO")
               continue;
            bool listsList = false;
            bool listsAddress = false;
            for (size_t i = 0; i < component.properties.size(); i++)
            {
               if (component.properties[i].name != "ATTENDEE")
                  continue;
               const AnsiString one = Imip::AddressOf(component.properties[i].value);
               if (one == list)
                  listsList = true;
               else if (one == address)
                  listsAddress = true;
            }
            if (!listsList || listsAddress)
               continue;

            ICalProperty property;
            property.name = "ATTENDEE";
            if (!name.IsEmpty())
               property.parameters.push_back(std::make_pair(AnsiString("CN"), name));
            property.parameters.push_back(std::make_pair(AnsiString("MEMBER"), "mailto:" + list));
            property.parameters.push_back(std::make_pair(AnsiString("PARTSTAT"), partstat));
            if (!sentBy.IsEmpty())
               property.parameters.push_back(std::make_pair(AnsiString("SENT-BY"), "mailto:" + Lower(sentBy)));
            property.value = "mailto:" + address;
            component.properties.push_back(property);
            added = true;
         }
         return added;
      }
   }

   ImipOrigin
   Imip::OriginOf(std::shared_ptr<const Message> original, const String &fileName)
   {
      ImipOrigin origin;
      if (original)
      {
         // A message an application sent through the send API records the
         // account its key sent as, for the domain rules; no session of that
         // account wrote it (Message::FlagSubmittedThroughSendApi).
         if (!original->GetSubmittedThroughSendApi())
            origin.submitterAccountId = original->GetSubmitterAccountID();
         origin.authDomains = Lower(Trimmed(Utf8Of(original->GetAuthDomains())));
      }
      // The From counts only beside a DMARC pass for mail from outside, so it
      // is read only then.
      if (origin.submitterAccountId <= 0 && !origin.authDomains.IsEmpty())
         origin.fromAddress = FromAddressOf(PersistentMessage::LoadHeader(fileName, false));
      return origin;
   }

   bool
   Imip::IsSentBy(const ImipOrigin &origin, const AnsiString &address)
   {
      const AnsiString wanted = AddressOf(address);
      if (wanted.IsEmpty() || wanted.Find("@") <= 0)
         return false;

      // A signed-in session of this server: the account, or an address it
      // may send as. What else the message says does not matter.
      if (origin.submitterAccountId > 0)
      {
         std::shared_ptr<const Account> submitter = CacheContainer::Instance()->GetAccount(origin.submitterAccountId);
         if (!submitter)
            return false;
         String reason;
         return SMTPConnection::SenderPermittedFor(submitter->GetAddress(), WideOf(wanted), reason);
      }

      // An address of this server's own is its account's to prove by signing
      // in: a DMARC pass for a hosted domain can come from any platform the
      // domain's SPF record names.
      if (IsLocalAddress(wanted))
         return false;

      return VouchedFromOutside(origin.authDomains, origin.fromAddress, wanted);
   }

   std::vector<AnsiString>
   Imip::ListedAddressesFor(const ICalComponent &tree, const AnsiString &address,
                            const std::function<bool(const AnsiString &listed, const AnsiString &named)> &sameMailbox)
   {
      std::vector<AnsiString> found;
      const AnsiString named = AddressOf(address);
      if (named.IsEmpty())
         return found;

      std::vector<AnsiString> listed;
      for (size_t c = 0; c < tree.children.size(); c++)
      {
         const ICalComponent &component = tree.children[c];
         if (component.name != "VEVENT" && component.name != "VTODO")
            continue;
         for (size_t i = 0; i < component.properties.size(); i++)
         {
            if (component.properties[i].name != "ATTENDEE")
               continue;
            const AnsiString one = AddressOf(component.properties[i].value);
            if (one.IsEmpty() || std::find(listed.begin(), listed.end(), one) != listed.end())
               continue;
            listed.push_back(one);
         }
      }

      // The address itself, when the event lists it: then it is the one the
      // answer is for, and no other spelling of the same mailbox is.
      if (std::find(listed.begin(), listed.end(), named) != listed.end())
      {
         found.push_back(named);
         return found;
      }

      // Otherwise the listed addresses that are the same mailbox: the
      // invitation went to an alias, and the answer came from the account.
      for (size_t i = 0; i < listed.size(); i++)
      {
         if (sameMailbox && sameMailbox(listed[i], named))
            found.push_back(listed[i]);
      }
      return found;
   }

   bool
   Imip::SameMailbox(const AnsiString &one, const AnsiString &other)
   {
      const AnsiString first = AddressOf(one);
      const AnsiString second = AddressOf(other);
      if (first.IsEmpty() || second.IsEmpty())
         return false;
      std::shared_ptr<const Account> a = AccountOf(first);
      std::shared_ptr<const Account> b = AccountOf(second);
      return a && b && a->GetID() == b->GetID();
   }

   bool
   Imip::IsListMember(const AnsiString &list, const AnsiString &address)
   {
      const AnsiString named = AddressOf(address);
      AnsiString wanted = AddressOf(list);
      if (named.IsEmpty() || wanted.IsEmpty() || named == wanted)
         return false;

      std::shared_ptr<DomainAliases> domainAliases = ObjectCache::Instance()->GetDomainAliases();
      String resolved = WideOf(wanted);
      if (domainAliases)
         resolved = domainAliases->ApplyAliasesOnAddress(resolved);
      std::shared_ptr<const DistributionList> distributionList = CacheContainer::Instance()->GetDistributionList(resolved);
      if (!distributionList || !distributionList->GetActive())
         return false;

      // The list's own record of the address - a confirmed row, or an account
      // its membership query answers - as the answer spells it, or as the
      // account it is (an alias, an alias domain's spelling) is spelt.
      if (DistributionListMembership::IsMember(distributionList, WideOf(named)))
         return true;
      std::shared_ptr<const Account> account = AccountOf(named);
      if (!account)
         return false;
      String primary = account->GetAddress();
      primary.ToLower();
      return Utf8Of(primary) != named && DistributionListMembership::IsMember(distributionList, primary);
   }

   bool
   Imip::VouchedFromOutside(const AnsiString &authDomains, const AnsiString &fromAddress, const AnsiString &address)
   {
      const AnsiString wanted = AddressOf(address);
      const AnsiString from = Lower(Trimmed(fromAddress));
      if (wanted.IsEmpty() || from.IsEmpty() || from != wanted)
         return false;

      // One address: one @, a local part and a domain, no space anywhere.
      const int at = wanted.Find("@");
      if (at <= 0 || at != wanted.ReverseFind('@') || at + 1 >= wanted.GetLength())
         return false;
      for (int i = 0; i < wanted.GetLength(); i++)
      {
         const char c = wanted.c_str()[i];
         if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',')
            return false;
      }

      // Whole tokens: "dmarc=example.com" vouches for example.com and for no
      // domain above, below or beside it.
      AnsiString tokens = Lower(authDomains);
      tokens.Replace(",", " ");
      tokens.Replace("\t", " ");
      const AnsiString padded = " " + tokens + " ";
      return padded.Find(" dmarc=" + wanted.Mid(at + 1) + " ") >= 0;
   }

   AnsiString
   Imip::FromAddressOf(const AnsiString &header)
   {
      if (header.IsEmpty())
         return "";

      MimeHeader mimeHeader;
      mimeHeader.Load(header.c_str(), header.GetLength(), true);

      // One From field: RFC 5322 section 3.6 allows no more, and the DMARC
      // check fails a message that has more.
      int fields = 0;
      AnsiString value;
      for (MimeField &field : mimeHeader.Fields())
      {
         const char *name = field.GetName();
         if (name && AnsiString(name).CompareNoCase("From") == 0)
         {
            fields++;
            const char *text = field.GetValue();
            value = text ? text : "";
         }
      }
      if (fields != 1)
         return "";

      // One mailbox, and the one the DMARC check read: it takes the first of
      // a list, with the comments removed, so a list or a comment the two
      // readers see differently names nobody.
      const String wide = WideOf(value);
      AddresslistParser parser;
      std::vector<std::shared_ptr<Address> > addresses = parser.ParseList(wide);
      if (addresses.size() != 1 || !addresses[0] || addresses[0]->sMailboxName.IsEmpty() || addresses[0]->sDomainName.IsEmpty())
         return "";
      const String listed = addresses[0]->sMailboxName + _T("@") + addresses[0]->sDomainName;
      const String checked = DMARC::ExtractAddressFromHeaderValue(wide);
      if (listed.CompareNoCase(checked) != 0)
         return "";

      AnsiString address = Lower(Trimmed(Utf8Of(listed)));
      if (address.Find("@") <= 0 || address.Find("@") != address.ReverseFind('@'))
         return "";
      for (int i = 0; i < address.GetLength(); i++)
      {
         const char c = address.c_str()[i];
         if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == '<' || c == '>' || c == '"' || c == '(' || c == ')')
            return "";
      }

      // And written as that one mailbox, with nothing in the field that
      // another program's reader - the sender's own mail system's, whose
      // reading DMARC vouches for - could take for another address.
      if (!OneMailboxSyntax(value, address))
         return "";
      return address;
   }

   bool
   Imip::ApplyToStored(ICalComponent &tree, const ImipMessage &message, const AnsiString &me,
                       const std::function<bool(const AnsiString &address)> &sentBy,
                       const std::function<bool(const AnsiString &attendee, const AnsiString &delegate)> &delegateSent,
                       const std::function<bool(const AnsiString &listed, const AnsiString &named)> &sameMailbox,
                       const std::function<bool(const AnsiString &list, const AnsiString &named)> &listMember,
                       size_t &refused)
   {
      refused = 0;
      const ICalComponent *master = MasterOf(tree);
      const AnsiString storedOrganiser = master ? AddressOf(master->Value("ORGANIZER")) : AnsiString("");

      if (message.method == "REPLY")
      {
         // The reply is the organiser's to file: only an event this account
         // organises takes an attendee's answer.
         if (!storedOrganiser.IsEmpty() && storedOrganiser != Lower(me))
            return false;

         // And only the answer of an attendee the event already lists, given
         // by that attendee - or by a delegate the attendee named, who is then
         // kept beside the answer as SENT-BY. A REPLY may name any attendee,
         // with any answer. The listed address is the one verified and
         // changed: an answer from another address of the same mailbox (the
         // invitation went to an alias) lets the sender change nothing it
         // could not change by naming the listed address itself.
         bool answered = false;
         for (size_t i = 0; i < message.attendees.size(); i++)
         {
            const ImipAttendee &attendee = message.attendees[i];
            const std::vector<AnsiString> listed = ListedAddressesFor(tree, attendee.address, sameMailbox);
            bool any = false;
            for (size_t l = 0; l < listed.size(); l++)
            {
               AnsiString keptSentBy;
               bool believed = false;
               if (sentBy(listed[l]))
                  believed = true;
               else if (!attendee.sentBy.IsEmpty() && delegateSent(listed[l], attendee.sentBy))
               {
                  believed = true;
                  keptSentBy = attendee.sentBy;
               }
               if (!believed)
                  continue;

               // Only where the event lists the attendee: an occurrence the
               // organiser left them out of stays so.
               SetPartstat(tree, listed[l], attendee.name, attendee.partstat, keptSentBy, false);
               any = true;
            }

            // An attendee the event does not list at all, invited through a
            // distribution list of this server's that it lists: added beside
            // the list, by the server's own record of who is in it, when they
            // sent the answer themselves or through their delegate.
            const AnsiString named = AddressOf(attendee.address);
            AnsiString memberSentBy;
            bool memberBelieved = false;
            if (listed.empty() && !named.IsEmpty() && listMember)
            {
               if (sentBy(named))
                  memberBelieved = true;
               else if (!attendee.sentBy.IsEmpty() && delegateSent(named, attendee.sentBy))
               {
                  memberBelieved = true;
                  memberSentBy = attendee.sentBy;
               }
            }
            if (memberBelieved)
            {
               std::vector<AnsiString> lists;
               for (size_t c = 0; c < tree.children.size(); c++)
               {
                  const ICalComponent &component = tree.children[c];
                  if (component.name != "VEVENT" && component.name != "VTODO")
                     continue;
                  for (size_t p = 0; p < component.properties.size(); p++)
                  {
                     if (component.properties[p].name != "ATTENDEE")
                        continue;
                     const AnsiString one = AddressOf(component.properties[p].value);
                     if (!one.IsEmpty() && one != named && std::find(lists.begin(), lists.end(), one) == lists.end())
                        lists.push_back(one);
                  }
               }

               // Beside each list that has them, on the components that list
               // it and do not list them yet.
               for (size_t k = 0; k < lists.size(); k++)
               {
                  if (listMember(lists[k], named) && AddListMember(tree, lists[k], named, attendee.name, attendee.partstat, memberSentBy))
                     any = true;
               }
            }

            if (!any)
            {
               refused++;
               continue;
            }
            answered = true;
         }
         return answered;
      }

      if (message.method == "CANCEL")
      {
         // A cancellation is the organiser's: the one the stored event names,
         // as the server verified who sent it.
         if (storedOrganiser.IsEmpty() || storedOrganiser != AddressOf(message.organizer) || !sentBy(message.organizer))
         {
            refused++;
            return false;
         }

         // Of occurrences, those alone (RFC 5546 section 3.2.5): taken out of
         // the copy, and the rest of the series kept. Until 6.3.6 the whole
         // series was marked cancelled. When they are everything the copy
         // holds, it is cancelled as a CANCEL of the whole event cancels it.
         if (CancelsOccurrences(message))
         {
            switch (TakeOutOccurrences(tree, message.tree))
            {
            case ImipOccurrences::NoneHeld:
               return false;
            case ImipOccurrences::Some:
               return true;
            case ImipOccurrences::All:
               break;
            }
         }

         MarkCancelled(tree);
         return true;
      }

      return false;
   }

   bool
   Imip::CancelsOccurrences(const ImipMessage &message)
   {
      bool any = false;
      for (size_t i = 0; i < message.tree.children.size(); i++)
      {
         const ICalComponent &component = message.tree.children[i];
         if (!IsEventLike(component))
            continue;
         if (!component.Property("RECURRENCE-ID"))
            return false;
         any = true;
      }
      return any;
   }

   ImipOccurrences
   Imip::OccurrencesHeld(const ICalComponent &stored, const ICalComponent &cancel)
   {
      return Held(stored, NamedOccurrences(cancel));
   }

   ImipOccurrences
   Imip::TakeOutOccurrences(ICalComponent &stored, const ICalComponent &cancel)
   {
      const std::vector<NamedOccurrence> named = NamedOccurrences(cancel);
      std::set<__int64> made;
      const ImipOccurrences held = Held(stored, named, &made);
      if (held != ImipOccurrences::Some)
         return held;

      // What the series makes is asked of the copy as it was, before any of
      // it is taken out.
      const ICalComponent before = stored;

      __int64 from = ICalendar::Forever;
      for (size_t i = 0; i < named.size(); i++)
      {
         if (named[i].andAfter)
            from = std::min(from, named[i].instant);
      }

      DropOverrides(stored, [&named](__int64 instant) { return Covers(named, instant); });

      ICalComponent *master = MutableSeriesMasterOf(stored);
      if (master)
      {
         // One EXDATE for each occurrence named alone that the series makes -
         // as Held found them in the copy as it was - before any named with
         // every one after it (those go below). The form of the DTSTART read
         // once: an EXDATE does not change it, and read for each it placed the
         // DTSTART again, its zone built afresh, once per occurrence named.
         const StartForm form = StartFormOf(*master, stored);
         for (const __int64 instant : made)
         {
            if (instant < from)
               AddExdate(*master, form, instant);
         }

         if (from != ICalendar::Forever)
         {
            DropRdatesFrom(*master, stored, from);
            EndRuleBefore(*master, before, from);

            // What the series still makes from there on - its DTSTART, which
            // an UNTIL before it may not take out, when it is that late and
            // an override or an RDATE before it is held - left out by EXDATE.
            std::vector<__int64> left;
            if (SeriesInstances(stored, from, ICalendar::Forever, 8, left))
            {
               for (size_t i = 0; i < left.size(); i++)
               {
                  if (left[i] >= from)
                     AddExdate(*master, stored, left[i]);
               }
            }
         }
      }

      return ImipOccurrences::Some;
   }

   bool
   Imip::AttendeeFor(const ImipMessage &message, const AnsiString &me, ImipAttendee &attendee)
   {
      std::shared_ptr<DomainAliases> domainAliases = ObjectCache::Instance()->GetDomainAliases();
      for (size_t i = 0; i < message.attendees.size(); i++)
      {
         String address;
         Unicode::MultiByteToWide(message.attendees[i].address, address);
         String primary = domainAliases ? domainAliases->ApplyAliasesOnAddress(address) : address;
         primary.ToLower();
         if (Utf8Of(primary) == me)
         {
            attendee = message.attendees[i];
            return true;
         }

         std::shared_ptr<const Alias> alias = CacheContainer::Instance()->GetAlias(primary);
         if (alias && alias->GetIsActive())
         {
            String value = alias->GetValue();
            value = domainAliases ? domainAliases->ApplyAliasesOnAddress(value) : value;
            value.ToLower();
            if (Utf8Of(value) == me)
            {
               attendee = message.attendees[i];
               return true;
            }
         }
      }
      return false;
   }

   int
   Imip::SequenceOf(const ICalComponent &component)
   {
      AnsiString text = Trimmed(component.Value("SEQUENCE"));
      if (text.IsEmpty())
         return 0;

      const long long value = strtoll(text.c_str(), nullptr, 10);
      return value > INT_MAX ? INT_MAX : (value < INT_MIN ? INT_MIN : static_cast<int>(value));
   }

   ImipKeepRule
   Imip::KeepRule(ImipProvenance provenance, bool inCalendar, int storedSequence, bool storedUnverified, int invitationSequence)
   {
      ImipKeepRule rule;
      if (!inCalendar)
      {
         // A new event: from anybody, as the message itself is delivered, and
         // marked unless its organiser was verified.
         rule.mark = provenance != ImipProvenance::FromOrganiser;
         return rule;
      }

      // The ORGANIZER the invitation names is only what it says, and anybody
      // who was mailed the UID can write the organiser's address there: what
      // is stored is replaced only when the organiser was verified, or by the
      // account's own answer.
      if (provenance == ImipProvenance::Unverified)
      {
         rule.refused = true;
         rule.useInvitation = false;
         return rule;
      }

      // An older invitation does not undo a newer - unless the newer is a copy
      // nobody verified and the organiser's own invitation has come: its
      // SEQUENCE is whatever its writer chose.
      if (storedSequence > invitationSequence)
         rule.useInvitation = provenance == ImipProvenance::FromOrganiser && storedUnverified;

      rule.mark = rule.useInvitation && provenance == ImipProvenance::AccountsAnswer &&
                  (storedUnverified || storedSequence < 0 || invitationSequence > storedSequence);
      return rule;
   }

   bool
   Imip::FiledUnverified(const ICalComponent &tree)
   {
      for (size_t c = 0; c < tree.children.size(); c++)
      {
         const ICalComponent &component = tree.children[c];
         if ((component.name == "VEVENT" || component.name == "VTODO") && component.Property(UnverifiedMark))
            return true;
      }
      return false;
   }

   bool
   Imip::KeepInvitation(__int64 accountId, const ImipMessage &invitation, bool createDefault, bool cancelled,
                        const AnsiString &address, const AnsiString &name, const AnsiString &partstat,
                        CalendarRecord &calendar, CalendarObjectRecord &record, AnsiString &problem,
                        ImipProvenance provenance)
   {
      if (invitation.uid.IsEmpty())
      {
         problem = "the invitation has no UID";
         return false;
      }
      String uid;
      Unicode::MultiByteToWide(invitation.uid, uid);

      // Where the UID already is, across the account's calendars.
      std::vector<CalendarRecord> calendars;
      if (!CalendarStore::ListCalendarsIfAny(accountId, calendars))
      {
         problem = "the calendars could not be read";
         return false;
      }
      bool inCalendar = false;
      CalendarObjectRecord stored;
      for (size_t c = 0; c < calendars.size() && !inCalendar; c++)
      {
         if (CalendarStore::FindByUid(accountId, calendars[c].id, uid, stored))
         {
            calendar = calendars[c];
            inCalendar = true;
         }
      }

      // The invitation's copy replaces what was stored when it is the same or
      // a later sequence, so a changed time reaches the calendar; an older
      // invitation does not undo a newer, unless the newer is a copy nobody
      // verified (KeepRule).
      ICalComponent storedTree;
      const ICalComponent *storedMaster = nullptr;
      int storedSequence = -1;
      bool storedUnverified = false;
      if (inCalendar)
      {
         AnsiString ignored;
         if (ICalendar::Parse(Utf8Of(stored.data), storedTree, ignored))
         {
            storedMaster = MasterOf(storedTree);
            storedUnverified = FiledUnverified(storedTree);
            // The stored event's organiser must be the invitation's. A UID is
            // mailed to every guest, so anybody who saw an invitation could
            // otherwise send a REQUEST under it, with themselves as organiser
            // and a higher SEQUENCE, and rewrite the event in the account's own
            // calendar on delivery. An event the account made itself has the
            // account, or nobody, as organiser and is refused the same way.
            //
            // But for the account's own answer to an invitation over a copy
            // marked as filed unverified: nobody verified the organiser that
            // copy names, so it is no more the organiser's than this one, and
            // the account has chosen this one - the copy is replaced, as if
            // the calendar had none, and what is written is marked as the
            // account's answer marks it. Until 6.3.6 a copy planted under the
            // UID, naming another organiser, before the real invitation came,
            // kept the real one out of the calendar on delivery - which it
            // still does, since a verified sender is not the UID's owner - and
            // made the real invitation's own card answer 422. A copy the
            // server did not mark (the organiser's, verified; the account's
            // own) stays refused, and so does a CANCEL.
            const bool otherOrganiser = storedMaster && AddressOf(storedMaster->Value("ORGANIZER")) != invitation.organizer;
            if (otherOrganiser && (provenance != ImipProvenance::AccountsAnswer || cancelled || !storedUnverified))
            {
               problem = "the stored event has another organiser";
               return false;
            }
            if (otherOrganiser)
               storedMaster = nullptr;
            else if (storedMaster)
               storedSequence = Imip::SequenceOf(*storedMaster);
         }
         else
            storedTree = invitation.tree;
      }

      const ImipKeepRule rule = KeepRule(provenance, inCalendar, storedSequence, storedUnverified, invitation.sequence);
      if (rule.refused)
      {
         // The ORGANIZER above is only what the invitation says, and anybody
         // who was mailed the UID can write the organiser's address there:
         // what is stored is replaced only when the caller verified that the
         // organiser sent it.
         problem = NotReplacedUnverified;
         return false;
      }

      // A CANCEL of occurrences of the copy its organiser holds: those taken
      // out, and the rest of the series kept with the answer set on it - not
      // the copy replaced by the CANCEL's events, which are the cancelled
      // occurrences alone and would have left the calendar holding them and
      // nothing else of the series. When they are all it holds it is
      // cancelled whole, as ApplyToStored cancels it.
      if (cancelled && storedMaster && CancelsOccurrences(invitation))
      {
         ICalComponent kept = storedTree;
         if (TakeOutOccurrences(kept, invitation.tree) == ImipOccurrences::All)
            MarkCancelled(kept);
         if (!partstat.IsEmpty())
            SetPartstat(kept, address, name, partstat);
         record = stored;
         return WriteObject(accountId, calendar.id, kept, record, false, problem);
      }

      const bool useInvitation = rule.useInvitation;

      // A resend at the same SEQUENCE carries NEEDS-ACTION for the account -
      // the organiser never received the answer, or sent the original again -
      // and RFC 5546 steps the sequence for neither a description nor an
      // attendee-list change; the answer already given is carried into the
      // replacement.
      AnsiString keptPartstat;
      AnsiString keptSentBy;
      if (storedMaster && useInvitation && storedSequence == invitation.sequence && !address.IsEmpty())
         ReadPartstat(*storedMaster, address, keptPartstat, keptSentBy);

      if (!useInvitation && !cancelled && partstat.IsEmpty())
      {
         // Nothing to change: the newer copy stays as it is.
         record = stored;
         return true;
      }

      ICalComponent tree = useInvitation ? invitation.tree : storedTree;
      // The stored copy is a plain object: the METHOD is the mail's. In one
      // pass: one at a time from the middle, an invitation of many METHOD
      // lines - a stranger's, filed on delivery - cost their square.
      tree.properties.erase(std::remove_if(tree.properties.begin(), tree.properties.end(), [](const ICalProperty &property)
         {
            return property.name == "METHOD";
         }), tree.properties.end());
      // The server's mark is the server's: what the invitation carried of it
      // goes, and it is set when nobody verified the organiser of what is
      // written. The stored copy, kept, keeps the mark it had.
      if (useInvitation)
         SetUnverifiedMark(tree, rule.mark);
      if (cancelled)
         MarkCancelled(tree);
      if (partstat.IsEmpty() && !keptPartstat.IsEmpty() && keptPartstat != "NEEDS-ACTION")
         SetPartstat(tree, address, name, keptPartstat, keptSentBy);
      if (!partstat.IsEmpty())
         SetPartstat(tree, address, name, partstat);

      if (!inCalendar)
      {
         if (createDefault)
         {
            if (!CalendarStore::EnsureDefault(accountId, calendar))
            {
               problem = "the calendar could not be read";
               return false;
            }
         }
         else if (!CalendarStore::DefaultIfAny(accountId, calendar))
         {
            problem = "the account has no calendar yet";
            return false;
         }
         stored = CalendarObjectRecord();
      }
      record = stored;
      return WriteObject(accountId, calendar.id, tree, record, !inCalendar, problem);
   }

   void
   Imip::FileOnDelivery(std::shared_ptr<const Account> account, const String &fileName, bool spam, std::shared_ptr<const Message> original)
   {
      try
      {
         // A message flagged as spam files nothing, whatever it carries: a
         // calendar part in spam is calendar spam, and a filed one would be
         // drawn on the page and counted as busy.
         if (!account || spam)
            return;
         AnsiString calendarText;
         if (!FindInFile(fileName, calendarText))
            return;

         ImipMessage message;
         AnsiString problem;
         if (!Read(calendarText, message, problem))
            return;
         if (message.method != "REPLY" && message.method != "CANCEL" && message.method != "REQUEST")
            return;
         if (message.uid.IsEmpty())
            return;

         String uid;
         Unicode::MultiByteToWide(message.uid, uid);

         // A room or a piece of equipment takes a REQUEST and a CANCEL by its
         // booking policy, decided after this delivery (ResourceBooking.h),
         // and nothing else: it organises nothing, so a REPLY is nobody's to
         // file in its calendar.
         if (account->IsResource())
         {
            if (message.method == "REQUEST" || message.method == "CANCEL")
               ResourceBooking::OnDelivery(account, message, fileName, original);
            return;
         }

         String me = account->GetAddress();
         me.ToLower();
         const AnsiString meUtf8 = Utf8Of(me);

         // Who sent it, as the server verified it (IsSentBy): the calendar
         // part says whatever its writer put there, and a UID is mailed to
         // every guest, so nothing in it changes an event already stored
         // unless it came from the one whose word the change is.
         const ImipOrigin origin = OriginOf(original, fileName);

         if (message.method == "REQUEST")
         {
            // An invitation, kept as it arrived in the default calendar of an
            // account that does not organise the event, so that the page shows
            // it as tentative and a delegate can answer it; an account with no
            // calendar row gets nothing, since a delivery is not a use.
            if (!message.organizer.IsEmpty() && message.organizer == meUtf8)
               return;
            // Only an invitation that names the account is the account's to
            // keep: a forwarded one, one to a list the account is in, and
            // anything sent to see what sticks name somebody else, or nobody.
            ImipAttendee attendee;
            if (!AttendeeFor(message, meUtf8, attendee))
            {
               LOG_DEBUG("iMIP: a REQUEST for " + uid + " does not name " + account->GetAddress() + " as a guest and was not filed.");
               return;
            }

            CalendarRecord calendar;
            if (!CalendarStore::DefaultIfAny(account->GetID(), calendar))
               return;
            String personName = account->GetPersonFirstName();
            if (!account->GetPersonLastName().IsEmpty())
               personName += (personName.IsEmpty() ? _T("") : _T(" ")) + account->GetPersonLastName();
            // A new invitation is filed from anybody, as it arrived and not
            // answered, as the message itself is delivered; a copy already
            // stored is replaced only by its organiser (KeepInvitation).
            const bool fromOrganiser = IsSentBy(origin, message.organizer);
            CalendarObjectRecord record;
            if (KeepInvitation(account->GetID(), message, false, false, meUtf8, Trimmed(Utf8Of(personName)), "", calendar, record, problem,
                               fromOrganiser ? ImipProvenance::FromOrganiser : ImipProvenance::Unverified))
            {
               LOG_DEBUG("iMIP: a REQUEST for " + uid + " was filed into the calendar of " + account->GetAddress() + ".");
            }
            else if (problem == NotReplacedUnverified)
            {
               LOG_APPLICATION("iMIP: a REQUEST for " + uid + " to " + account->GetAddress() +
                  " was not applied to the event in its calendar: the server could not verify that the event's organiser sent it. It was delivered as an ordinary message.");
            }
            else
            {
               LOG_DEBUG("iMIP: a REQUEST for " + uid + " was not filed into the calendar of " + account->GetAddress() + ": " + String(problem));
            }
            return;
         }

         // The verification ApplyToStored asks for, from the message's origin:
         // an address the message was sent by, and a delegate of an attendee's
         // - an account of this server with a delegate share on one of the
         // attendee's calendars - that submitted it, signed in.
         const std::function<bool(const AnsiString &)> sentBy = [&origin](const AnsiString &address)
         {
            return IsSentBy(origin, address);
         };
         const std::function<bool(const AnsiString &, const AnsiString &)> delegateSent = [&origin](const AnsiString &attendee, const AnsiString &delegate)
         {
            return origin.submitterAccountId > 0 && IsSentBy(origin, delegate) && IsDelegateOf(attendee, origin.submitterAccountId);
         };
         // And whether an address a REPLY names and one the event lists are
         // one mailbox of this server's (an alias the invitation went to).
         const std::function<bool(const AnsiString &, const AnsiString &)> sameMailbox = [](const AnsiString &listed, const AnsiString &named)
         {
            return SameMailbox(listed, named);
         };
         // And whether an address a REPLY names is a member of a distribution
         // list of this server's that the event lists.
         const std::function<bool(const AnsiString &, const AnsiString &)> listMember = [](const AnsiString &list, const AnsiString &named)
         {
            return IsListMember(list, named);
         };

         std::vector<CalendarRecord> calendars;
         if (!CalendarStore::ListCalendars(account->GetID(), calendars))
            return;

         for (size_t c = 0; c < calendars.size(); c++)
         {
            CalendarObjectRecord stored;
            if (!CalendarStore::FindByUid(account->GetID(), calendars[c].id, uid, stored))
               continue;

            ICalComponent tree;
            AnsiString storedProblem;
            if (!ICalendar::Parse(Utf8Of(stored.data), tree, storedProblem))
               continue;

            size_t refused = 0;
            const bool changed = ApplyToStored(tree, message, meUtf8, sentBy, delegateSent, sameMailbox, listMember, refused);
            if (refused > 0 && message.method == "REPLY")
            {
               String count;
               count.Format(_T("%d"), static_cast<int>(refused));
               LOG_APPLICATION("iMIP: a REPLY for " + uid + " to " + account->GetAddress() + " left " + count +
                  " answer(s) in the event as they were: the event does not list the attendee, or the server could not verify that the attendee, or a delegate of theirs, sent it.");
            }
            else if (refused > 0)
            {
               LOG_APPLICATION("iMIP: a CANCEL for " + uid + " to " + account->GetAddress() +
                  " was not applied to the event in its calendar: it does not name the event's organiser, or the server could not verify that the organiser sent it. It was delivered as an ordinary message.");
            }
            if (!changed)
               continue;

            AnsiString serialized = ICalendar::Serialize(tree);
            ICalendar::Summary summary;
            if (!ICalendar::Summarize(tree, summary, storedProblem))
               continue;

            CalendarObjectRecord record = stored;
            String data;
            Unicode::MultiByteToWide(serialized, data);
            record.data = data;
            record.etag = Dav::Wide(Dav::EtagOf(serialized));
            record.start = summary.start;
            record.end = summary.end;
            record.first = summary.first;
            record.last = summary.last;

            __int64 token = 0;
            if (CalendarStore::Update(account->GetID(), calendars[c].id, stored.id, record, token))
            {
               LOG_DEBUG("iMIP: a " + String(message.method) + " for " + uid + " was filed into the calendar of " + account->GetAddress() + ".");
            }
         }
      }
      catch (const boost::thread_interrupted&)
      {
         // A stop's interruption, not a fault: see WorkQueue::Stop.
         boost::this_thread::disable_interruption disabled;
         LOG_DEBUG("iMIP: a stop of the servers interrupted the filing of a calendar message delivered to " + (account ? account->GetAddress() : String()) + "; the message is in the mailbox.");
         throw;
      }
      catch (...)
      {
         // Delivery has happened; nothing here may undo it. Said as
         // ResourceBooking::OnDelivery says it for a room, but with a code of its
         // own - the rooms' HM7434 fires on no server without a room - and once an
         // hour at most: a message that makes the filing fail can be sent again
         // and again.
         static std::atomic<long long> lastReport(0);
         long long last = lastReport.load();
         const long long now = (long long) std::time(nullptr);
         if ((last == 0 || now < last || now - last >= 3600) && lastReport.compare_exchange_strong(last, now))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7555, "Imip::FileOnDelivery",
               _T("A calendar reply, cancellation or invitation delivered to ") + (account ? account->GetAddress() : String()) +
               _T(" could not be filed into its calendar: an unexpected error stopped it. The message is in the mailbox. Said once an hour at most."));
         }
      }
   }

   void
   ImipTester::Test()
   {
      const char *crlf = "\r\n";
      struct FromCase
      {
         const char *header;
         const char *expected;
         const char *what;
      };
      const FromCase fromCases[] =
      {
         { "From: Olga Organiser <olga@partner.example>\r\nTo: <guest@example.com>\r\n", "olga@partner.example", "a display name and an address" },
         { "Subject: x\r\nfrom: OLGA@Partner.Example\r\n", "olga@partner.example", "a bare address, lower-cased, the field named in any case" },
         { "From: \"Doe, Jane\" <jane@partner.example>\r\n", "jane@partner.example", "a quoted display name with a comma" },
         { "From: \"ceo@bank.example\" <mallory@evil.example>\r\n", "", "a display name that holds another address" },
         { "From: \"olga@partner.example\" <olga@partner.example>\r\n", "olga@partner.example", "a display name that is the address, as Outlook writes a name it does not know" },
         { "From: =?utf-8?B?T2xnYSDDmHJnYW5pc2Vy?= <olga@partner.example>\r\n", "olga@partner.example", "an encoded display name" },
         { "From: <olga@partner.example> <mallory@partner.example>\r\n", "", "two <addresses>" },
         { "From: \"Olga\" <olga@partner.example>mallory@partner.example\r\n", "", "text after the >" },
         { "From: mallory@partner.example <olga@partner.example>\r\n", "", "an address written as the display name, unquoted" },
         { "From: \"'olga@partner.example'\" <olga@partner.example>\r\n", "olga@partner.example", "the address as the display name in single quotes, as Outlook writes one" },
         { "From: 'olga@partner.example' <olga@partner.example>\r\n", "olga@partner.example", "the address as the display name, unquoted" },
         { "From: \"'mallory@partner.example'\" <olga@partner.example>\r\n", "", "another address as the display name in single quotes" },
         { "From: \"<mallory@partner.example>\" <olga@partner.example>\r\n", "", "brackets in the display name" },
         { "From: olga@partner.example (mallory@partner.example)\r\n", "", "an address in a comment" },
         { "From: Olga (mallory@partner.example) <olga@partner.example>\r\n", "", "an address in a comment that both readers pass over" },
         { "From: \"Olga <Organiser>\" <olga@partner.example>\r\n", "", "brackets in a quoted display name with no address in it" },
         { "From: <olga@partner.example> (the organiser)\r\n", "olga@partner.example", "a comment after the address" },
         { "From: olga@partner.example,\r\n", "", "a list's comma after the one mailbox" },
         { "From: Olga Organiser\r\n <olga@partner.example>\r\n", "olga@partner.example", "a folded field" },
         { "From: olga@partner.example\r\nFrom: mallory@evil.example\r\n", "", "two From fields" },
         { "From: olga@partner.example, mallory@partner.example\r\n", "", "a list of two mailboxes" },
         { "From: olga@partner.example (<mallory@evil.example>)\r\n", "", "a comment the two readers see differently" },
         { "From: Undisclosed recipients:;\r\n", "", "a group with nobody in it" },
         { "From: olga\r\n", "", "no domain" },
         { "To: <guest@example.com>\r\nSubject: no sender\r\n", "", "no From" },
         { "", "", "no header" },
      };
      for (size_t i = 0; i < sizeof(fromCases) / sizeof(fromCases[0]); i++)
      {
         const AnsiString header = AnsiString(fromCases[i].header) + crlf;
         const AnsiString read = Imip::FromAddressOf(fromCases[i].header[0] ? header : AnsiString());
         if (read != fromCases[i].expected)
            throw std::logic_error(std::string("ImipTester: FromAddressOf read \"") + read.c_str() + "\" where \"" + fromCases[i].expected + "\" was expected, for " + fromCases[i].what + ".");
      }

      struct VouchCase
      {
         const char *authDomains;
         const char *from;
         const char *address;
         bool expected;
         const char *what;
      };
      const VouchCase vouchCases[] =
      {
         { "dmarc=partner.example", "olga@partner.example", "olga@partner.example", true, "DMARC for the domain, and the From is the address" },
         { "dmarc=partner.example", "olga@partner.example", "mailto:OLGA@Partner.Example", true, "the address as a calendar writes it" },
         { "DMARC=Partner.Example", "olga@partner.example", "olga@partner.example", true, "the record in another case" },
         { "dmarc=other.example,dmarc=partner.example", "olga@partner.example", "olga@partner.example", true, "the domain's token among others, after a comma" },
         { "dmarc=other.example dmarc=partner.example", "olga@partner.example", "olga@partner.example", true, "the domain's token among others, after a space" },
         { "dmarc=partner.example", "mallory@partner.example", "olga@partner.example", false, "another sender at the same domain" },
         { "dmarc=partner.example", "", "olga@partner.example", false, "no From the header could be read by" },
         { "dmarc=evil.example", "olga@partner.example", "olga@partner.example", false, "DMARC for another domain" },
         { "dmarc=example", "olga@partner.example", "olga@partner.example", false, "DMARC for the parent domain" },
         { "dmarc=sub.partner.example", "olga@partner.example", "olga@partner.example", false, "DMARC for a subdomain" },
         { "dmarc=partner.example.evil", "olga@partner.example", "olga@partner.example", false, "DMARC for a longer name" },
         { "dmarc=xpartner.example", "olga@partner.example", "olga@partner.example", false, "DMARC for a name that ends the same" },
         { "spf=partner.example", "olga@partner.example", "olga@partner.example", false, "an SPF pass alone" },
         { "", "olga@partner.example", "olga@partner.example", false, "nothing recorded" },
         { "dmarc=partner.example", "olga@partner.example", "", false, "an empty address" },
         { "dmarc=partner.example", "a@b@partner.example", "a@b@partner.example", false, "an address with two @" },
         { "dmarc=partner.example", "olga @partner.example", "olga @partner.example", false, "an address with a space" },
      };
      for (size_t i = 0; i < sizeof(vouchCases) / sizeof(vouchCases[0]); i++)
      {
         const bool vouched = Imip::VouchedFromOutside(vouchCases[i].authDomains, vouchCases[i].from, vouchCases[i].address);
         if (vouched != vouchCases[i].expected)
            throw std::logic_error(std::string("ImipTester: VouchedFromOutside answered ") + (vouched ? "true" : "false") + " for " + vouchCases[i].what + ".");
      }

      // What an invitation does to a copy of its UID, by who stands behind
      // it: a copy nobody verified holds off no verified organiser by its
      // SEQUENCE, and is never replaced by another nobody verified.
      struct KeepCase
      {
         ImipProvenance provenance;
         bool inCalendar;
         int storedSequence;
         bool storedUnverified;
         int invitationSequence;
         bool refused;
         bool useInvitation;
         bool mark;
         const char *what;
      };
      const KeepCase keepCases[] =
      {
         { ImipProvenance::Unverified, false, -1, false, 0, false, true, true, "a new invitation nobody verified is filed, marked" },
         { ImipProvenance::Unverified, true, 0, false, 5, true, false, false, "an invitation nobody verified replaces no stored copy" },
         { ImipProvenance::Unverified, true, 9, true, 10, true, false, false, "an invitation nobody verified replaces no marked copy either" },
         { ImipProvenance::FromOrganiser, false, -1, false, 0, false, true, false, "the verified organiser's new invitation is filed unmarked" },
         { ImipProvenance::FromOrganiser, true, 9, true, 0, false, true, false, "the verified organiser's invitation replaces a marked copy at a later SEQUENCE" },
         { ImipProvenance::FromOrganiser, true, 9, false, 0, false, false, false, "the verified organiser's older invitation does not undo a newer copy nobody marked" },
         { ImipProvenance::FromOrganiser, true, 1, true, 1, false, true, false, "the verified organiser's invitation at the same SEQUENCE replaces a marked copy, unmarked" },
         { ImipProvenance::AccountsAnswer, false, -1, false, 0, false, true, true, "an answer to an invitation not in the calendar files it marked" },
         { ImipProvenance::AccountsAnswer, true, 1, false, 1, false, true, false, "an answer at the stored copy's SEQUENCE leaves it unmarked" },
         { ImipProvenance::AccountsAnswer, true, 1, false, 2, false, true, true, "an answer to a later version nobody verified marks what it writes" },
         { ImipProvenance::AccountsAnswer, true, 9, true, 0, false, false, false, "an answer to an older invitation keeps the newer copy, marked as it was" },
         { ImipProvenance::AccountsAnswer, true, 1, true, 1, false, true, true, "an answer leaves a marked copy marked" },
         { ImipProvenance::AccountsAnswer, true, -1, false, 0, false, true, true, "an answer over a stored copy that does not read marks what it writes" },
      };
      for (size_t i = 0; i < sizeof(keepCases) / sizeof(keepCases[0]); i++)
      {
         const KeepCase &k = keepCases[i];
         const ImipKeepRule rule = Imip::KeepRule(k.provenance, k.inCalendar, k.storedSequence, k.storedUnverified, k.invitationSequence);
         if (rule.refused != k.refused || rule.useInvitation != k.useInvitation || rule.mark != k.mark)
            throw std::logic_error(std::string("ImipTester: KeepRule: ") + k.what + " did not hold.");
      }
      {
         ICalComponent marked;
         ICalComponent plain;
         AnsiString problem;
         const AnsiString head = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//ImipTester//EN\r\nBEGIN:VEVENT\r\nUID:mark-0001@example.com\r\nDTSTAMP:20260928T080000Z\r\nDTSTART:20261026T090000Z\r\n";
         if (!ICalendar::Parse(head + "X-HMAILSERVER-IMIP-UNVERIFIED:TRUE\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", marked, problem) ||
             !ICalendar::Parse(head + "X-OTHER-VENDOR:TRUE\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", plain, problem))
            throw std::logic_error(std::string("ImipTester: the marked copies did not parse: ") + problem.c_str());
         if (!Imip::FiledUnverified(marked) || Imip::FiledUnverified(plain))
            throw std::logic_error("ImipTester: FiledUnverified did not read the server's mark, or read another.");
      }

      // SEQUENCE past an int's range, which RFC 5545 does not allow but a
      // message can carry: held at that end on both platforms. atoi read
      // 4294967297 as 1 on Linux, so the verified organiser's invitation at that
      // SEQUENCE was taken for one older than a stored copy at 2 and dropped.
      {
         struct SequenceCase
         {
            const char *value;
            int expected;
         };
         const SequenceCase sequenceCases[] =
         {
            { "", 0 },
            { "0", 0 },
            { "7", 7 },
            { "  12 ", 12 },
            { "2147483647", INT_MAX },
            { "2147483648", INT_MAX },
            { "4294967297", INT_MAX },
            { "99999999999999999999", INT_MAX },
            { "-4294967297", INT_MIN },
            { "x", 0 },
         };
         for (const SequenceCase &c : sequenceCases)
         {
            const AnsiString text = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//ImipTester//EN\r\nMETHOD:REQUEST\r\nBEGIN:VEVENT\r\nUID:sequence-0001@example.com\r\n"
               "DTSTAMP:20260928T080000Z\r\nDTSTART:20261026T090000Z\r\nDTEND:20261026T100000Z\r\nORGANIZER:mailto:olga@partner.example\r\n" +
               (c.value[0] ? AnsiString("SEQUENCE:") + c.value + "\r\n" : AnsiString()) + "END:VEVENT\r\nEND:VCALENDAR\r\n";
            ImipMessage message;
            AnsiString problem;
            if (!Imip::Read(text, message, problem))
               throw std::logic_error(std::string("ImipTester: an invitation at SEQUENCE ") + c.value + " did not read: " + problem.c_str());
            if (message.sequence != c.expected)
               throw std::logic_error(std::string("ImipTester: SEQUENCE ") + c.value + " was read as " + std::to_string(message.sequence) + ".");
            const ICalComponent *master = nullptr;
            for (const ICalComponent &child : message.tree.children)
            {
               if (child.name == "VEVENT")
                  master = &child;
            }
            if (!master || Imip::SequenceOf(*master) != c.expected)
               throw std::logic_error(std::string("ImipTester: SequenceOf read SEQUENCE ") + c.value + " otherwise than Read.");
         }

         const ImipKeepRule later = Imip::KeepRule(ImipProvenance::FromOrganiser, true, 2, false, INT_MAX);
         if (!later.useInvitation)
            throw std::logic_error("ImipTester: the verified organiser's invitation at SEQUENCE 4294967297 did not replace a copy at 2.");
      }

      // What a delivered CANCEL or REPLY does to a stored copy of its event,
      // with the verification given - who the message was sent by, and
      // which delegate of an attendee's.
      const AnsiString self = "self@example.com";
      const AnsiString olga = "olga@partner.example";
      const AnsiString guest = "guest@example.com";
      const AnsiString coGuest = "coguest@example.com";
      const AnsiString deputy = "deputy@example.com";
      const AnsiString uidLine = "UID:board-0001@example.com\r\n";

      auto storedEvent = [&](const AnsiString &organiser) -> ICalComponent
      {
         const AnsiString text = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//ImipTester//EN\r\nBEGIN:VEVENT\r\n" + uidLine +
            "DTSTAMP:20260928T080000Z\r\nDTSTART:20261026T090000Z\r\nDTEND:20261026T100000Z\r\nSUMMARY:Board\r\nSEQUENCE:0\r\n" +
            (organiser.IsEmpty() ? AnsiString() : "ORGANIZER:mailto:" + organiser + "\r\n") +
            "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + guest + "\r\nATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + coGuest + "\r\n" +
            "END:VEVENT\r\nEND:VCALENDAR\r\n";
         ICalComponent tree;
         AnsiString problem;
         if (!ICalendar::Parse(text, tree, problem))
            throw std::logic_error(std::string("ImipTester: the stored event did not parse: ") + problem.c_str());
         return tree;
      };
      auto deliveredMessage = [&](const AnsiString &method, const AnsiString &organiser, const AnsiString &attendeeLines) -> ImipMessage
      {
         const AnsiString text = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//ImipTester//EN\r\nMETHOD:" + method + "\r\nBEGIN:VEVENT\r\n" + uidLine +
            "DTSTAMP:20260928T080000Z\r\nDTSTART:20261026T090000Z\r\nDTEND:20261026T100000Z\r\nSEQUENCE:1\r\nORGANIZER:mailto:" + organiser + "\r\n" +
            attendeeLines + "END:VEVENT\r\nEND:VCALENDAR\r\n";
         ImipMessage message;
         AnsiString problem;
         if (!Imip::Read(text, message, problem))
            throw std::logic_error(std::string("ImipTester: the delivered object did not parse: ") + problem.c_str());
         return message;
      };
      auto attendeeParameter = [](const ICalComponent &tree, const AnsiString &address, const char *parameter, bool &listed) -> AnsiString
      {
         listed = false;
         const ICalComponent *master = MasterOf(tree);
         if (!master)
            return "";
         for (size_t i = 0; i < master->properties.size(); i++)
         {
            const ICalProperty &property = master->properties[i];
            if (property.name != "ATTENDEE" || Imip::AddressOf(property.value) != address)
               continue;
            listed = true;
            if (AnsiString(parameter) == "SENT-BY")
               return Imip::AddressOf(property.Parameter(parameter));
            AnsiString value = Trimmed(property.Parameter(parameter));
            value.ToUpper();
            return value;
         }
         return "";
      };
      auto partstatOf = [&](const ICalComponent &tree, const AnsiString &address) -> AnsiString
      {
         bool listed = false;
         const AnsiString partstat = attendeeParameter(tree, address, "PARTSTAT", listed);
         return listed ? partstat : AnsiString("(not listed)");
      };
      auto sentByOf = [&](const ICalComponent &tree, const AnsiString &address) -> AnsiString
      {
         bool listed = false;
         return attendeeParameter(tree, address, "SENT-BY", listed);
      };
      auto isCancelled = [](const ICalComponent &tree) -> bool
      {
         const ICalComponent *master = MasterOf(tree);
         AnsiString status = master ? Trimmed(master->Value("STATUS")) : AnsiString();
         status.ToUpper();
         return status == "CANCELLED";
      };
      auto sentOnlyBy = [](const AnsiString &who) -> std::function<bool(const AnsiString &)>
      {
         return [who](const AnsiString &address) { return !who.IsEmpty() && Imip::AddressOf(address) == who; };
      };
      const std::function<bool(const AnsiString &)> nobody = [](const AnsiString &) { return false; };
      const std::function<bool(const AnsiString &, const AnsiString &)> noDelegate = [](const AnsiString &, const AnsiString &) { return false; };
      const std::function<bool(const AnsiString &, const AnsiString &)> noAlias = [](const AnsiString &, const AnsiString &) { return false; };
      const std::function<bool(const AnsiString &, const AnsiString &)> noList = [](const AnsiString &, const AnsiString &) { return false; };
      auto expect = [](bool holds, const char *what)
      {
         if (!holds)
            throw std::logic_error(std::string("ImipTester: ApplyToStored: ") + what + ".");
      };
      size_t refused = 0;

      // CANCEL, at a guest: the stored event is Olga's.
      const AnsiString guestLines = "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + guest + "\r\n";
      {
         ICalComponent tree = storedEvent(olga);
         expect(!Imip::ApplyToStored(tree, deliveredMessage("CANCEL", olga, guestLines), guest, nobody, noDelegate, noAlias, noList, refused) && !isCancelled(tree) && refused == 1,
            "a CANCEL in the organiser's name that nobody verified cancelled the event");
         expect(!Imip::ApplyToStored(tree, deliveredMessage("CANCEL", olga, guestLines), guest, sentOnlyBy(coGuest), noDelegate, noAlias, noList, refused) && !isCancelled(tree),
            "a CANCEL in the organiser's name sent by a co-guest cancelled the event");
         expect(!Imip::ApplyToStored(tree, deliveredMessage("CANCEL", "mallory@partner.example", guestLines), guest, sentOnlyBy("mallory@partner.example"), noDelegate, noAlias, noList, refused) &&
            !isCancelled(tree), "a CANCEL naming another organiser, sent by that organiser, cancelled the event");
         expect(Imip::ApplyToStored(tree, deliveredMessage("CANCEL", olga, guestLines), guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused) && isCancelled(tree) && refused == 0,
            "the organiser's own CANCEL did not cancel the event");
      }
      {
         ICalComponent tree = storedEvent("");
         expect(!Imip::ApplyToStored(tree, deliveredMessage("CANCEL", olga, guestLines), guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused) && !isCancelled(tree),
            "a CANCEL cancelled an event that names no organiser");
      }

      // CANCEL of occurrences, at a guest (RFC 5546 section 3.2.5): those
      // occurrences go, and the rest of the series stays - where until 6.3.6
      // the whole series was marked cancelled. What the copy shows is read
      // back as the calendar reads it, by expanding it: each occurrence's
      // start, with an x when it is marked cancelled.
      {
         auto parsed = [](const AnsiString &text) -> ICalComponent
         {
            ICalComponent tree;
            AnsiString problem;
            if (!ICalendar::Parse(text, tree, problem))
               throw std::logic_error(std::string("ImipTester: a calendar object did not parse: ") + problem.c_str());
            return tree;
         };
         auto shown = [](const ICalComponent &tree) -> AnsiString
         {
            __int64 from = 0;
            __int64 to = 0;
            ICalendar::ParseUtc("20261001T000000Z", from);
            ICalendar::ParseUtc("20270101T000000Z", to);
            std::vector<ICalInstance> instances;
            bool truncated = false;
            AnsiString problem;
            if (!ICalendar::Expand(tree, from, to, 100, instances, truncated, problem))
               return "(does not expand: " + problem + ")";
            ICalendar::Summary summary;
            if (!ICalendar::Summarize(tree, summary, problem))
               return "(would not be stored: " + problem + ")";
            AnsiString out;
            for (size_t i = 0; i < instances.size(); i++)
            {
               AnsiString status = Trimmed(tree.children[instances[i].component].Value("STATUS"));
               status.ToUpper();
               out += (out.IsEmpty() ? "" : "|") + ICalendar::FormatUtc(instances[i].start) + (status == "CANCELLED" ? "x" : "");
            }
            return out;
         };
         const AnsiString seriesUid = "UID:series-0001@example.com\r\n";
         const AnsiString people = "ORGANIZER:mailto:" + olga + "\r\nATTENDEE;PARTSTAT=ACCEPTED:mailto:" + guest + "\r\n";
         const AnsiString head = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//ImipTester//EN\r\n";
         // A weekly series of four from 2 November, its second occurrence moved
         // to the afternoon by an override.
         auto weekly = [&]() -> ICalComponent
         {
            return parsed(head +
               "BEGIN:VEVENT\r\n" + seriesUid + "DTSTAMP:20260928T080000Z\r\nDTSTART:20261102T090000Z\r\nDTEND:20261102T100000Z\r\n"
               "RRULE:FREQ=WEEKLY;COUNT=4\r\nSUMMARY:Weekly\r\nSEQUENCE:0\r\n" + people + "END:VEVENT\r\n"
               "BEGIN:VEVENT\r\n" + seriesUid + "RECURRENCE-ID:20261109T090000Z\r\nDTSTAMP:20260928T080000Z\r\nDTSTART:20261109T140000Z\r\n"
               "DTEND:20261109T150000Z\r\nSUMMARY:Weekly, later\r\nSEQUENCE:0\r\n" + people + "END:VEVENT\r\nEND:VCALENDAR\r\n");
         };
         // The organiser's CANCEL of the occurrences each line names.
         auto cancelOf = [&](const std::vector<AnsiString> &recurrenceIds) -> ImipMessage
         {
            AnsiString text = head + "METHOD:CANCEL\r\n";
            for (size_t i = 0; i < recurrenceIds.size(); i++)
            {
               text += "BEGIN:VEVENT\r\n" + seriesUid + recurrenceIds[i] + "\r\nDTSTAMP:20260929T080000Z\r\n"
                  "SEQUENCE:1\r\nSTATUS:CANCELLED\r\n" + people + "END:VEVENT\r\n";
            }
            text += "END:VCALENDAR\r\n";
            ImipMessage message;
            AnsiString problem;
            if (!Imip::Read(text, message, problem))
               throw std::logic_error(std::string("ImipTester: the CANCEL did not parse: ") + problem.c_str());
            return message;
         };
         const AnsiString wholeSeries = "20261102T090000Z|20261109T140000Z|20261116T090000Z|20261123T090000Z";
         if (shown(weekly()) != wholeSeries)
            throw std::logic_error(std::string("ImipTester: the stored series reads as ") + shown(weekly()).c_str());

         struct OccurrenceCase
         {
            std::vector<AnsiString> recurrenceIds;
            bool applied;
            const char *shows;
            const char *why;
         };
         const OccurrenceCase occurrenceCases[] =
         {
            { { "RECURRENCE-ID:20261116T090000Z" }, true, "20261102T090000Z|20261109T140000Z|20261123T090000Z",
              "a CANCEL of one occurrence took out more, or less, than that occurrence" },
            { { "RECURRENCE-ID:20261109T090000Z" }, true, "20261102T090000Z|20261116T090000Z|20261123T090000Z",
              "a CANCEL of the occurrence an override moved left the override, or the occurrence, in the series" },
            { { "RECURRENCE-ID:20261116T090000Z", "RECURRENCE-ID:20261123T090000Z" }, true, "20261102T090000Z|20261109T140000Z",
              "a CANCEL of two occurrences did not take out both" },
            { { "RECURRENCE-ID;RANGE=THISANDFUTURE:20261116T090000Z" }, true, "20261102T090000Z|20261109T140000Z",
              "a CANCEL of an occurrence and every one after it did not end the series before it" },
            { { "RECURRENCE-ID;RANGE=THISANDFUTURE:20261109T090000Z" }, true, "20261102T090000Z",
              "a CANCEL of the overridden occurrence and those after it left the override, or one after it" },
            { { "RECURRENCE-ID;RANGE=THISANDFUTURE:20261102T090000Z" }, true, "20261102T090000Zx|20261109T140000Zx|20261116T090000Zx|20261123T090000Zx",
              "a CANCEL from the first occurrence on did not cancel the whole event, as a CANCEL of the event does" },
            { { "RECURRENCE-ID;RANGE=THISANDFUTURE:20261130T090000Z" }, false, wholeSeries,
              "a CANCEL from after the series' last occurrence changed it" },
            { { "RECURRENCE-ID:20261103T090000Z" }, false, wholeSeries,
              "a CANCEL of an occurrence the series does not have changed it" },
            { { "RECURRENCE-ID;TZID=Europe/London:20261116T090000" }, true, "20261102T090000Z|20261109T140000Z|20261123T090000Z",
              "a CANCEL whose RECURRENCE-ID is written in a zone was not matched by the instant it names" },
         };
         for (const OccurrenceCase &k : occurrenceCases)
         {
            ICalComponent tree = weekly();
            const ImipMessage cancel = cancelOf(k.recurrenceIds);
            expect(Imip::CancelsOccurrences(cancel), "a CANCEL whose every event has a RECURRENCE-ID was not taken for one of occurrences");
            const bool applied = Imip::ApplyToStored(tree, cancel, guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused);
            if (applied != k.applied || shown(tree) != k.shows || refused != 0)
               throw std::logic_error(std::string("ImipTester: ApplyToStored: ") + k.why + ": the copy shows " + shown(tree).c_str() +
                                      " where " + k.shows + " was expected.");
            if (k.applied && Imip::OccurrencesHeld(tree, cancel.tree) != (shown(tree).Find("x") >= 0 ? ImipOccurrences::All : ImipOccurrences::NoneHeld))
               throw std::logic_error(std::string("ImipTester: OccurrencesHeld: ") + k.why + ": the copy that took the CANCEL still reads as holding the occurrences.");
         }

         // Nobody verified as the organiser: the series stays as it was.
         {
            ICalComponent tree = weekly();
            expect(!Imip::ApplyToStored(tree, cancelOf({ "RECURRENCE-ID:20261116T090000Z" }), guest, nobody, noDelegate, noAlias, noList, refused) &&
                   shown(tree) == wholeSeries && refused == 1,
               "a CANCEL of an occurrence that nobody verified changed the series");
         }

         // A CANCEL with an event that has no RECURRENCE-ID is of the whole event.
         expect(!Imip::CancelsOccurrences(deliveredMessage("CANCEL", olga, guestLines)), "a CANCEL of the whole event was taken for one of occurrences");

         // What is written into the series: the EXDATE in the DTSTART's form
         // - a day for a series of days, floating for a floating series, and
         // UTC for one in a zone, which is the instant the zone reads - and
         // the RRULE ended with an UNTIL in place of its COUNT.
         auto valueOf = [](const ICalComponent &tree, const char *name) -> AnsiString
         {
            const ICalComponent *master = MasterOf(tree);
            AnsiString out;
            for (size_t i = 0; master && i < master->properties.size(); i++)
            {
               const ICalProperty &property = master->properties[i];
               if (property.name != name)
                  continue;
               AnsiString one = property.value;
               for (size_t p = 0; p < property.parameters.size(); p++)
                  one = property.parameters[p].first + "=" + property.parameters[p].second + ":" + one;
               out += (out.IsEmpty() ? "" : "|") + one;
            }
            return out;
         };
         {
            ICalComponent days = parsed(head +
               "BEGIN:VEVENT\r\n" + seriesUid + "DTSTAMP:20260928T080000Z\r\nDTSTART;VALUE=DATE:20261102\r\nDTEND;VALUE=DATE:20261103\r\n"
               "RRULE:FREQ=DAILY;COUNT=3\r\nSUMMARY:Away\r\n" + people + "END:VEVENT\r\nEND:VCALENDAR\r\n");
            expect(Imip::ApplyToStored(days, cancelOf({ "RECURRENCE-ID;VALUE=DATE:20261103" }), guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused) &&
                   shown(days) == "20261102T000000Z|20261104T000000Z" && valueOf(days, "EXDATE") == "VALUE=DATE:20261103",
               "a CANCEL of one day of a series of days did not take it out as a day");

            ICalComponent floating = parsed(head +
               "BEGIN:VEVENT\r\n" + seriesUid + "DTSTAMP:20260928T080000Z\r\nDTSTART:20261102T090000\r\nDTEND:20261102T100000\r\n"
               "RRULE:FREQ=WEEKLY;COUNT=3\r\nSUMMARY:Floating\r\n" + people + "END:VEVENT\r\nEND:VCALENDAR\r\n");
            expect(Imip::ApplyToStored(floating, cancelOf({ "RECURRENCE-ID;RANGE=THISANDFUTURE:20261109T090000" }), guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused) &&
                   shown(floating) == "20261102T090000Z" && valueOf(floating, "RRULE") == "FREQ=WEEKLY;UNTIL=20261109T085959",
               "a CANCEL from an occurrence of a floating series on did not end it with a floating UNTIL in place of its COUNT");

            ICalComponent london = parsed(head +
               "BEGIN:VEVENT\r\n" + seriesUid + "DTSTAMP:20260928T080000Z\r\nDTSTART;TZID=Europe/London:20261019T090000\r\n"
               "DTEND;TZID=Europe/London:20261019T093000\r\nRRULE:FREQ=WEEKLY;COUNT=3\r\nSUMMARY:Standup\r\n" + people + "END:VEVENT\r\nEND:VCALENDAR\r\n");
            expect(Imip::ApplyToStored(london, cancelOf({ "RECURRENCE-ID;TZID=Europe/London:20261026T090000" }), guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused) &&
                   shown(london) == "20261019T080000Z|20261102T090000Z" && valueOf(london, "EXDATE") == "20261026T090000Z",
               "a CANCEL of an occurrence of a series in a zone, after the clocks went back, did not take out that occurrence by its instant");

            // An RDATE from the occurrence on goes, and a COUNT that ended
            // before it is not made an UNTIL that would add occurrences.
            ICalComponent withDates = parsed(head +
               "BEGIN:VEVENT\r\n" + seriesUid + "DTSTAMP:20260928T080000Z\r\nDTSTART:20261102T090000Z\r\nDTEND:20261102T100000Z\r\n"
               "RRULE:FREQ=WEEKLY;COUNT=2\r\nRDATE:20261120T090000Z,20261127T090000Z\r\nSUMMARY:Dates\r\n" + people + "END:VEVENT\r\nEND:VCALENDAR\r\n");
            expect(Imip::ApplyToStored(withDates, cancelOf({ "RECURRENCE-ID;RANGE=THISANDFUTURE:20261127T090000Z" }), guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused) &&
                   shown(withDates) == "20261102T090000Z|20261109T090000Z|20261120T090000Z" && valueOf(withDates, "RRULE") == "FREQ=WEEKLY;COUNT=2" &&
                   valueOf(withDates, "RDATE") == "20261120T090000Z",
               "a CANCEL from an RDATE on did not take out that RDATE alone, or made the COUNT an UNTIL");
         }

         // A guest invited to one occurrence holds that occurrence alone: its
         // CANCEL cancels everything the copy holds, as a CANCEL of the event does.
         {
            ICalComponent one = parsed(head +
               "BEGIN:VEVENT\r\n" + seriesUid + "RECURRENCE-ID:20261116T090000Z\r\nDTSTAMP:20260928T080000Z\r\nDTSTART:20261116T090000Z\r\n"
               "DTEND:20261116T100000Z\r\nSUMMARY:Weekly, with a guest\r\nSEQUENCE:0\r\n" + people + "END:VEVENT\r\nEND:VCALENDAR\r\n");
            expect(Imip::ApplyToStored(one, cancelOf({ "RECURRENCE-ID:20261116T090000Z" }), guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused) &&
                   shown(one) == "20261116T090000Zx",
               "a CANCEL of the one occurrence a guest holds did not cancel it");
         }

         // What a card's "remove" takes out: the occurrences, and the event only
         // when they are all it holds.
         {
            ICalComponent tree = weekly();
            expect(Imip::TakeOutOccurrences(tree, cancelOf({ "RECURRENCE-ID:20261123T090000Z" }).tree) == ImipOccurrences::Some &&
                   shown(tree) == "20261102T090000Z|20261109T140000Z|20261116T090000Z",
               "taking out one occurrence took out more, or less");
            expect(Imip::TakeOutOccurrences(tree, cancelOf({ "RECURRENCE-ID:20261123T090000Z" }).tree) == ImipOccurrences::NoneHeld &&
                   shown(tree) == "20261102T090000Z|20261109T140000Z|20261116T090000Z",
               "an occurrence taken out already was taken out again");
            expect(Imip::TakeOutOccurrences(tree, cancelOf({ "RECURRENCE-ID;RANGE=THISANDFUTURE:20261102T090000Z" }).tree) == ImipOccurrences::All &&
                   shown(tree) == "20261102T090000Z|20261109T140000Z|20261116T090000Z",
               "occurrences that are all the copy holds were not answered All, or the copy was changed");
         }

         // A CANCEL may name as many occurrences as a calendar object holds
         // components, and finding whether the series makes one walks its rule
         // from the DTSTART: that is asked of one expansion over the span they
         // cover, not of two for each. A daily series from 1900, 1,500 of its
         // occurrences in 2099 named: those, and no other, taken out in the
         // time of a few of its expansions - measured here, at the best of
         // five, so that a slower machine, or one emulating another, is held
         // to the same count - where two for each occurrence named, as until
         // 6.3.6, took 3,000 (4.8 s in the CI image on linix, at 1.2 ms an
         // expansion, where the whole of this tester takes a quarter of a
         // second).
         {
            ICalComponent daily = parsed(head +
               "BEGIN:VEVENT\r\n" + seriesUid + "DTSTAMP:20260928T080000Z\r\nDTSTART:19000101T090000Z\r\nDTEND:19000101T100000Z\r\n"
               "RRULE:FREQ=DAILY\r\nSUMMARY:Every day\r\n" + people + "END:VEVENT\r\nEND:VCALENDAR\r\n");
            __int64 first = 0;
            ICalendar::ParseUtc("20990101T090000Z", first);
            const __int64 named = 1500;
            std::vector<AnsiString> recurrenceIds;
            for (__int64 i = 0; i < named; i++)
               recurrenceIds.push_back("RECURRENCE-ID:" + ICalendar::FormatUtc(first + i * 86400));
            const ImipMessage many = cancelOf(recurrenceIds);

            double expansion = 1e9;
            for (int attempt = 0; attempt < 5; attempt++)
            {
               std::vector<ICalInstance> one;
               bool cut = false;
               AnsiString ignored;
               const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
               ICalendar::Expand(daily, first - 1, first + 1, 50, one, cut, ignored);
               expansion = std::min(expansion, std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
            }

            const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
            const bool applied = Imip::ApplyToStored(daily, many, guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

            // The day before the first named and the day after the last, and
            // nothing between them.
            std::vector<ICalInstance> left;
            bool truncated = false;
            AnsiString problem;
            const bool expanded = ICalendar::Expand(daily, first - 86400, first + (named + 1) * 86400, 2000, left, truncated, problem);
            const double margin = HM_TEST_TIME_BOUND(250) / 1000.0;
            AnsiString took;
            took.Format("%.3f s, where one expansion of the series takes %.4f s", seconds, expansion);
            AnsiString allowed;
            allowed.Format("%.2f s", margin);
            AnsiString count;
            count.Format("%d", static_cast<int>(left.size()));
            if (!applied || !expanded || left.size() != 2 || seconds >= 50 * expansion + margin)
               throw std::logic_error(std::string("ImipTester: ApplyToStored: a CANCEL naming 1,500 occurrences of a daily series took ") +
                                      took.c_str() + " (less than 50 expansions and " + allowed.c_str() + " expected), or left " + count.c_str() +
                                      " occurrences of those and the two beside them where 2 were expected.");
         }

         // A span holding more occurrences than one expansion gives: a daily
         // series, its first, its 12,000th and its 20,500th occurrences named,
         // 56 years apart, each taken out - the last two by an expansion from
         // the first one the one before it did not reach - and the day after
         // each kept.
         {
            ICalComponent spread = parsed(head +
               "BEGIN:VEVENT\r\n" + seriesUid + "DTSTAMP:20260928T080000Z\r\nDTSTART:20270101T090000Z\r\nDTEND:20270101T100000Z\r\n"
               "RRULE:FREQ=DAILY\r\nSUMMARY:Every day for decades\r\n" + people + "END:VEVENT\r\nEND:VCALENDAR\r\n");
            __int64 start = 0;
            ICalendar::ParseUtc("20270101T090000Z", start);
            const __int64 picks[] = { 0, 11999, 20499 };
            std::vector<AnsiString> recurrenceIds;
            for (const __int64 pick : picks)
               recurrenceIds.push_back("RECURRENCE-ID:" + ICalendar::FormatUtc(start + pick * 86400));
            expect(Imip::ApplyToStored(spread, cancelOf(recurrenceIds), guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused),
               "a CANCEL of three occurrences of a daily series, further apart than one expansion reaches, changed nothing");
            for (const __int64 pick : picks)
            {
               const __int64 at = start + pick * 86400;
               std::vector<ICalInstance> around;
               bool truncated = false;
               AnsiString problem;
               expect(ICalendar::Expand(spread, at - 1, at + 86401, 10, around, truncated, problem) && around.size() == 1 && around[0].start == at + 86400,
                  "a CANCEL of occurrences further apart than one expansion reaches did not take out each of them, and that alone");
            }
         }

         // A copy whose series holds as many EXDATEs as an invitation's
         // megabyte has room for - its first 58,000 days out - and a CANCEL of
         // 17 occurrences spread over the 40,000 days after them. An expansion
         // of the 10,000 days after those out costs what one of the series
         // without them does and a reading of its EXDATEs, a few readings of
         // the copy at most; and those 17 are taken out, the day after each
         // kept, in the time of a few such expansions. Measured here, at the
         // best of three, so that a slower machine, or one emulating another,
         // is held to the same counts. Until 6.3.6 each instance an expansion
         // made was looked for among every EXDATE in turn: an expansion of this
         // copy took 0.3 s in the CI image on linix, where reading it takes
         // 0.008 s, and taking these occurrences out of it 1.5 s of a delivery
         // thread.
         {
            __int64 dayOne = 0;
            ICalendar::ParseUtc("19900101T090000Z", dayOne);
            const __int64 aDay = 86400;
            const __int64 out = 58000;
            AnsiString exdates = "EXDATE:";
            for (__int64 day = 0; day < out; day++)
               exdates += AnsiString(day > 0 ? "," : "") + ICalendar::FormatUtc(dayOne + day * aDay);
            auto dailyWith = [&](const AnsiString &extra) -> AnsiString
            {
               return head + "BEGIN:VEVENT\r\n" + seriesUid + "DTSTAMP:20260928T080000Z\r\nDTSTART:19900101T090000Z\r\nDTEND:19900101T100000Z\r\n"
                  "RRULE:FREQ=DAILY\r\n" + extra + "SUMMARY:Every day\r\n" + people + "END:VEVENT\r\nEND:VCALENDAR\r\n";
            };
            const AnsiString crowdedText = dailyWith(exdates + "\r\n");
            ICalComponent crowded = parsed(crowdedText);
            const ICalComponent plain = parsed(dailyWith(""));

            auto bestOfThree = [](const std::function<void()> &work) -> double
            {
               double best = 1e9;
               for (int attempt = 0; attempt < 3; attempt++)
               {
                  const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
                  work();
                  best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
               }
               return best;
            };
            const __int64 afterOut = dayOne + out * aDay;
            size_t madeCrowded = 0;
            size_t madePlain = 0;
            auto expansionOf = [&](const ICalComponent &tree, size_t &made) -> std::function<void()>
            {
               return [&tree, &made, afterOut]()
               {
                  std::vector<ICalInstance> instances;
                  bool truncated = false;
                  AnsiString problem;
                  ICalendar::Expand(tree, afterOut - 1, ICalendar::Forever, 10000, instances, truncated, problem);
                  made = instances.size();
               };
            };
            const double reading = bestOfThree([&crowdedText]()
            {
               ICalComponent again;
               AnsiString ignored;
               ICalendar::Parse(crowdedText, again, ignored);
            });
            const double plainExpansion = bestOfThree(expansionOf(plain, madePlain));
            const double crowdedExpansion = bestOfThree(expansionOf(crowded, madeCrowded));
            const double expansionMargin = HM_TEST_TIME_BOUND(20) / 1000.0;
            AnsiString costs;
            costs.Format("%.4f s for 10,000 days after 58,000 EXDATEs, %.4f s without them, %.4f s to read the copy", crowdedExpansion, plainExpansion,
                          reading);
            AnsiString expansionAllowed;
            expansionAllowed.Format("%.2f s", expansionMargin);
            if (madeCrowded != madePlain || madeCrowded < 9000 || crowdedExpansion >= plainExpansion + 4 * reading + expansionMargin)
               throw std::logic_error(std::string("ImipTester: an expansion of a series with 58,000 EXDATEs took ") + costs.c_str() +
                                      " (less than the series without them, four readings and " + expansionAllowed.c_str() +
                                      " expected), or did not make the days the series without them makes.");

            const __int64 picks = 17;
            std::vector<AnsiString> recurrenceIds;
            for (__int64 k = 0; k < picks; k++)
               recurrenceIds.push_back("RECURRENCE-ID:" + ICalendar::FormatUtc(afterOut + k * 2500 * aDay));
            const ImipMessage spreadCancel = cancelOf(recurrenceIds);

            const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
            const ImipOccurrences tookOut = Imip::TakeOutOccurrences(crowded, spreadCancel.tree);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

            // The first, the middle and the last named: each out, the day after
            // it kept.
            bool each = true;
            for (const __int64 k : { static_cast<__int64>(0), picks / 2, picks - 1 })
            {
               const __int64 at = afterOut + k * 2500 * aDay;
               std::vector<ICalInstance> around;
               bool truncated = false;
               AnsiString problem;
               each = each && ICalendar::Expand(crowded, at - 1, at + aDay + 1, 10, around, truncated, problem) && around.size() == 1 &&
                      around[0].start == at + aDay;
            }
            const double cancelMargin = HM_TEST_TIME_BOUND(250) / 1000.0;
            AnsiString took;
            took.Format("%.3f s, where %s", seconds, costs.c_str());
            AnsiString cancelAllowed;
            cancelAllowed.Format("%.2f s", cancelMargin);
            if (tookOut != ImipOccurrences::Some || !each || seconds >= 25 * crowdedExpansion + cancelMargin)
               throw std::logic_error(std::string("ImipTester: TakeOutOccurrences: a CANCEL of 17 occurrences of a series with 58,000 EXDATEs took ") +
                                      took.c_str() + " (less than 25 of those expansions and " + cancelAllowed.c_str() +
                                      " expected), or did not take out each of them alone.");
         }

         // A copy that has lost occurrences already - it took earlier CANCELs,
         // each an EXDATE - is asked what else it holds past them: an
         // expansion stops at a number of the rule's own occurrences before
         // its EXDATEs take any out. The first occurrence of a weekly series
         // cancelled after its second and third were leaves the three after
         // them, not cancelled, both on delivery and by a card's remove; and a
         // CANCEL from the 71st day of a daily series on, whose 71st to 140th
         // days are out already, ends the series before it, where taking 64 of
         // them for all there is found nothing to cancel.
         {
            const AnsiString gapsText = head +
               "BEGIN:VEVENT\r\n" + seriesUid + "DTSTAMP:20260928T080000Z\r\nDTSTART:20261102T090000Z\r\nDTEND:20261102T100000Z\r\n"
               "RRULE:FREQ=WEEKLY;COUNT=6\r\nEXDATE:20261109T090000Z,20261116T090000Z\r\nSUMMARY:Weekly\r\nSEQUENCE:0\r\n" + people +
               "END:VEVENT\r\nEND:VCALENDAR\r\n";
            const AnsiString afterFirst = "20261123T090000Z|20261130T090000Z|20261207T090000Z";
            const ImipMessage first = cancelOf({ "RECURRENCE-ID:20261102T090000Z" });
            ICalComponent filed = parsed(gapsText);
            expect(Imip::OccurrencesHeld(filed, first.tree) == ImipOccurrences::Some,
               "a series whose next occurrences are out already was read as holding the first of them and nothing else");
            expect(Imip::ApplyToStored(filed, first, guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused) && shown(filed) == afterFirst,
               "a CANCEL of the first occurrence of a series whose next two are out already cancelled the series, or took out more");
            ICalComponent removed = parsed(gapsText);
            expect(Imip::TakeOutOccurrences(removed, first.tree) == ImipOccurrences::Some && shown(removed) == afterFirst,
               "a card's remove of the first occurrence of a series whose next two are out already would remove the series");

            AnsiString dailyText = head +
               "BEGIN:VEVENT\r\n" + seriesUid + "DTSTAMP:20260928T080000Z\r\nDTSTART:20260501T090000Z\r\nDTEND:20260501T100000Z\r\n"
               "RRULE:FREQ=DAILY;COUNT=160\r\nSUMMARY:Every day\r\n" + people;
            __int64 dayOne = 0;
            ICalendar::ParseUtc("20260501T090000Z", dayOne);
            const __int64 aDay = 86400;
            for (__int64 day = 70; day < 140; day++)
               dailyText += "EXDATE:" + ICalendar::FormatUtc(dayOne + day * aDay) + "\r\n";
            dailyText += "END:VEVENT\r\nEND:VCALENDAR\r\n";
            ICalComponent daily = parsed(dailyText);
            const AnsiString from71 = "RECURRENCE-ID;RANGE=THISANDFUTURE:" + ICalendar::FormatUtc(dayOne + 70 * aDay);
            expect(Imip::ApplyToStored(daily, cancelOf({ from71 }), guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused) &&
                   valueOf(daily, "RRULE") == "FREQ=DAILY;UNTIL=20260710T085959Z",
               "a CANCEL from an occurrence on, past 70 occurrences out already, did not end the series before it");
            std::vector<ICalInstance> kept;
            bool truncated = false;
            AnsiString problem;
            expect(ICalendar::Expand(daily, dayOne - 1, ICalendar::Forever, 1000, kept, truncated, problem) && kept.size() == 70 &&
                   kept.back().start == dayOne + 69 * aDay,
               "a CANCEL from an occurrence on, past 70 occurrences out already, left one after it, or took one before it");
         }

         // Cancellations as calendar programs write them, each from the
         // organiser of the copy its own invitation filed: one occurrence
         // taken out, matched by the instant its RECURRENCE-ID names in the
         // CANCEL's own VTIMEZONE (a quoted Windows zone name from Exchange,
         // an Olson name from Google and Apple), across the October clock
         // change; an all-day one by its day; and Exchange's CANCEL of the
         // whole series, which still cancels everything that is left.
         {
            auto withCrlf = [](AnsiString text) -> AnsiString
            {
               text.Replace("\n", "\r\n");
               return text;
            };
            auto clientMessage = [&](const AnsiString &text) -> ImipMessage
            {
               ImipMessage message;
               AnsiString problem;
               if (!Imip::Read(withCrlf(text), message, problem))
                  throw std::logic_error(std::string("ImipTester: a client's CANCEL did not parse: ") + problem.c_str());
               return message;
            };
            const AnsiString exchangeZone =
               "BEGIN:VTIMEZONE\nTZID:GMT Standard Time\nBEGIN:STANDARD\nDTSTART:16010101T020000\nTZOFFSETFROM:+0100\nTZOFFSETTO:+0000\n"
               "RRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=10\nEND:STANDARD\nBEGIN:DAYLIGHT\nDTSTART:16010101T010000\nTZOFFSETFROM:+0000\n"
               "TZOFFSETTO:+0100\nRRULE:FREQ=YEARLY;INTERVAL=1;BYDAY=-1SU;BYMONTH=3\nEND:DAYLIGHT\nEND:VTIMEZONE\n";
            const AnsiString exchangeUid = "UID:040000008200E00074C5B7101A82E00800000000E07A6E3C5B4FDC010000000000000000100000003B2B5E9D0C1F2A4C8D6E7F8091A2B3C4\n";
            const AnsiString exchangePeople =
               "ORGANIZER;CN=Olga Organiser:mailto:" + olga + "\nATTENDEE;ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION;RSVP=TRUE;CN=" + guest + ":mailto:" + guest + "\n";
            const AnsiString exchangeRule = "RRULE:FREQ=WEEKLY;UNTIL=20261130T100000Z;INTERVAL=1;BYDAY=MO;WKST=MO\n";
            ICalComponent exchange = parsed(withCrlf("BEGIN:VCALENDAR\nPRODID:Microsoft Exchange Server 2010\nVERSION:2.0\n" + exchangeZone +
               "BEGIN:VEVENT\n" + exchangePeople + "DESCRIPTION;LANGUAGE=en-GB:\\n\n" + exchangeRule + exchangeUid +
               "SUMMARY;LANGUAGE=en-GB:Weekly review\nDTSTART;TZID=GMT Standard Time:20261019T100000\nDTEND;TZID=GMT Standard Time:20261019T103000\n"
               "CLASS:PUBLIC\nPRIORITY:5\nDTSTAMP:20260928T080000Z\nTRANSP:OPAQUE\nSTATUS:CONFIRMED\nSEQUENCE:0\nLOCATION;LANGUAGE=en-GB:Room 1\n"
               "X-MICROSOFT-CDO-APPT-SEQUENCE:0\nX-MICROSOFT-CDO-BUSYSTATUS:TENTATIVE\nX-MICROSOFT-CDO-INTENDEDSTATUS:BUSY\n"
               "X-MICROSOFT-CDO-ALLDAYEVENT:FALSE\nX-MICROSOFT-CDO-IMPORTANCE:1\nX-MICROSOFT-CDO-INSTTYPE:1\nX-MICROSOFT-LOCATIONS:[]\n"
               "END:VEVENT\nEND:VCALENDAR\n"));
            const AnsiString sevenMondays = "20261019T090000Z|20261026T100000Z|20261102T100000Z|20261109T100000Z|20261116T100000Z|20261123T100000Z|20261130T100000Z";
            expect(shown(exchange) == sevenMondays, "Exchange's weekly series does not read as its seven Mondays across the clock change");

            const ImipMessage exchangeOne = clientMessage("BEGIN:VCALENDAR\nMETHOD:CANCEL\nPRODID:Microsoft Exchange Server 2010\nVERSION:2.0\n" + exchangeZone +
               "BEGIN:VEVENT\n" + exchangePeople + "DESCRIPTION;LANGUAGE=en-GB:\\n\nRECURRENCE-ID;TZID=\"GMT Standard Time\":20261026T100000\n" + exchangeUid +
               "SUMMARY;LANGUAGE=en-GB:Canceled: Weekly review\nDTSTART;TZID=\"GMT Standard Time\":20261026T100000\n"
               "DTEND;TZID=\"GMT Standard Time\":20261026T103000\nCLASS:PUBLIC\nPRIORITY:1\nDTSTAMP:20260929T080000Z\nTRANSP:TRANSPARENT\n"
               "STATUS:CANCELLED\nSEQUENCE:1\nLOCATION;LANGUAGE=en-GB:Room 1\nX-MICROSOFT-CDO-APPT-SEQUENCE:1\nX-MICROSOFT-CDO-BUSYSTATUS:FREE\n"
               "X-MICROSOFT-CDO-INTENDEDSTATUS:FREE\nX-MICROSOFT-CDO-ALLDAYEVENT:FALSE\nX-MICROSOFT-CDO-IMPORTANCE:2\nX-MICROSOFT-CDO-INSTTYPE:3\n"
               "END:VEVENT\nEND:VCALENDAR\n");
            expect(Imip::CancelsOccurrences(exchangeOne) && Imip::OccurrencesHeld(exchange, exchangeOne.tree) == ImipOccurrences::Some,
               "Exchange's CANCEL of one occurrence was not read as one the copy holds among others");
            expect(Imip::ApplyToStored(exchange, exchangeOne, guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused) && refused == 0 &&
                   shown(exchange) == "20261019T090000Z|20261102T100000Z|20261109T100000Z|20261116T100000Z|20261123T100000Z|20261130T100000Z" &&
                   valueOf(exchange, "EXDATE") == "20261026T100000Z" && Imip::OccurrencesHeld(exchange, exchangeOne.tree) == ImipOccurrences::NoneHeld,
               "Exchange's CANCEL of the occurrence after the clock change, its RECURRENCE-ID in a quoted Windows zone, did not take out that occurrence alone");

            const ImipMessage exchangeAll = clientMessage("BEGIN:VCALENDAR\nMETHOD:CANCEL\nPRODID:Microsoft Exchange Server 2010\nVERSION:2.0\n" + exchangeZone +
               "BEGIN:VEVENT\n" + exchangePeople + exchangeRule + exchangeUid +
               "SUMMARY;LANGUAGE=en-GB:Canceled: Weekly review\nDTSTART;TZID=GMT Standard Time:20261019T100000\nDTEND;TZID=GMT Standard Time:20261019T103000\n"
               "CLASS:PUBLIC\nPRIORITY:1\nDTSTAMP:20260930T080000Z\nTRANSP:TRANSPARENT\nSTATUS:CANCELLED\nSEQUENCE:2\nX-MICROSOFT-CDO-INSTTYPE:1\n"
               "END:VEVENT\nEND:VCALENDAR\n");
            expect(!Imip::CancelsOccurrences(exchangeAll) &&
                   Imip::ApplyToStored(exchange, exchangeAll, guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused) &&
                   shown(exchange) == "20261019T090000Zx|20261102T100000Zx|20261109T100000Zx|20261116T100000Zx|20261123T100000Zx|20261130T100000Zx",
               "Exchange's CANCEL of the whole series did not cancel what was left of it");

            const AnsiString olsonZone =
               "BEGIN:VTIMEZONE\nTZID:Europe/London\nX-LIC-LOCATION:Europe/London\nBEGIN:DAYLIGHT\nTZOFFSETFROM:+0000\nTZOFFSETTO:+0100\nTZNAME:BST\n"
               "DTSTART:19700329T010000\nRRULE:FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU\nEND:DAYLIGHT\nBEGIN:STANDARD\nTZOFFSETFROM:+0100\nTZOFFSETTO:+0000\n"
               "TZNAME:GMT\nDTSTART:19701025T020000\nRRULE:FREQ=YEARLY;BYMONTH=10;BYDAY=-1SU\nEND:STANDARD\nEND:VTIMEZONE\n";
            const AnsiString googleUid = "UID:4h1k2j3l4m5n6o7p8q9r0s1t2u@google.com\n";
            const AnsiString googlePeople =
               "ORGANIZER;CN=Olga Organiser:mailto:" + olga + "\n"
               "ATTENDEE;CUTYPE=INDIVIDUAL;ROLE=REQ-PARTICIPANT;PARTSTAT=NEEDS-ACTION;RSVP=TRUE;CN=" + guest + ";X-NUM-GUESTS=0:mailto:" + guest + "\n"
               "ATTENDEE;CUTYPE=INDIVIDUAL;ROLE=REQ-PARTICIPANT;PARTSTAT=ACCEPTED;RSVP=TRUE;CN=Olga Organiser;X-NUM-GUESTS=0:mailto:" + olga + "\n";
            ICalComponent google = parsed(withCrlf("BEGIN:VCALENDAR\nPRODID:-//Google Inc//Google Calendar 70.9054//EN\nVERSION:2.0\nCALSCALE:GREGORIAN\n" + olsonZone +
               "BEGIN:VEVENT\nDTSTART;TZID=Europe/London:20261019T100000\nDTEND;TZID=Europe/London:20261019T103000\nRRULE:FREQ=WEEKLY;WKST=MO;COUNT=7;BYDAY=MO\n"
               "DTSTAMP:20260928T080000Z\n" + googlePeople + googleUid + "CREATED:20260928T075900Z\nDESCRIPTION:\nLAST-MODIFIED:20260928T080000Z\nLOCATION:\n"
               "SEQUENCE:0\nSTATUS:CONFIRMED\nSUMMARY:Weekly review\nTRANSP:OPAQUE\nEND:VEVENT\nEND:VCALENDAR\n"));
            expect(shown(google) == sevenMondays, "Google's weekly series does not read as its seven Mondays across the clock change");
            const ImipMessage googleOne = clientMessage("BEGIN:VCALENDAR\nPRODID:-//Google Inc//Google Calendar 70.9054//EN\nVERSION:2.0\nCALSCALE:GREGORIAN\nMETHOD:CANCEL\n" +
               olsonZone + "BEGIN:VEVENT\nDTSTART;TZID=Europe/London:20261102T100000\nDTEND;TZID=Europe/London:20261102T103000\nDTSTAMP:20260929T080000Z\n" +
               googlePeople + googleUid + "RECURRENCE-ID;TZID=Europe/London:20261102T100000\nCREATED:20260928T075900Z\nDESCRIPTION:\n"
               "LAST-MODIFIED:20260929T080000Z\nLOCATION:\nSEQUENCE:1\nSTATUS:CANCELLED\nSUMMARY:Weekly review\nTRANSP:OPAQUE\nEND:VEVENT\nEND:VCALENDAR\n");
            expect(Imip::ApplyToStored(google, googleOne, guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused) && refused == 0 &&
                   shown(google) == "20261019T090000Z|20261026T100000Z|20261109T100000Z|20261116T100000Z|20261123T100000Z|20261130T100000Z",
               "Google's CANCEL of one occurrence did not take out that occurrence alone");

            const AnsiString appleZone =
               "BEGIN:VTIMEZONE\nTZID:Europe/London\nBEGIN:DAYLIGHT\nDTSTART:19810329T010000\nRRULE:FREQ=YEARLY;BYDAY=-1SU;BYMONTH=3\nTZNAME:BST\n"
               "TZOFFSETFROM:+0000\nTZOFFSETTO:+0100\nEND:DAYLIGHT\nBEGIN:STANDARD\nDTSTART:19961027T020000\nRRULE:FREQ=YEARLY;BYDAY=-1SU;BYMONTH=10\n"
               "TZNAME:GMT\nTZOFFSETFROM:+0100\nTZOFFSETTO:+0000\nEND:STANDARD\nEND:VTIMEZONE\n";
            const AnsiString appleUid = "UID:2D8A3F7E-1B4C-4E5A-9F0D-6C7B8A9E0F12\n";
            const AnsiString appleGuest = "ATTENDEE;CN=" + guest + ";CUTYPE=INDIVIDUAL;EMAIL=" + guest +
               ";PARTSTAT=NEEDS-ACTION;ROLE=REQ-PARTICIPANT;RSVP=TRUE:mailto:" + guest + "\n";
            const AnsiString appleOrganiser = "ORGANIZER;CN=\"Olga Organiser\";EMAIL=" + olga + ":mailto:" + olga + "\n";
            ICalComponent apple = parsed(withCrlf("BEGIN:VCALENDAR\nCALSCALE:GREGORIAN\nPRODID:-//Apple Inc.//iOS 18.0//EN\nVERSION:2.0\n" + appleZone +
               "BEGIN:VEVENT\n" + appleGuest + "ATTENDEE;CN=\"Olga Organiser\";CUTYPE=INDIVIDUAL;EMAIL=" + olga + ";PARTSTAT=ACCEPTED;ROLE=CHAIR:mailto:" + olga + "\n"
               "CREATED:20260928T075900Z\nDTEND;TZID=Europe/London:20261019T103000\nDTSTAMP:20260928T080000Z\nDTSTART;TZID=Europe/London:20261019T100000\n"
               "LAST-MODIFIED:20260928T080000Z\n" + appleOrganiser + "RRULE:FREQ=WEEKLY;COUNT=7\nSEQUENCE:0\nSUMMARY:Weekly review\nTRANSP:OPAQUE\n" + appleUid +
               "END:VEVENT\nEND:VCALENDAR\n"));
            expect(shown(apple) == sevenMondays, "Apple's weekly series does not read as its seven Mondays across the clock change");
            const ImipMessage appleOne = clientMessage("BEGIN:VCALENDAR\nCALSCALE:GREGORIAN\nMETHOD:CANCEL\nPRODID:-//Apple Inc.//iOS 18.0//EN\nVERSION:2.0\n" + appleZone +
               "BEGIN:VEVENT\n" + appleGuest + "DTEND;TZID=Europe/London:20261109T103000\nDTSTAMP:20260929T080000Z\nDTSTART;TZID=Europe/London:20261109T100000\n" +
               appleOrganiser + "RECURRENCE-ID;TZID=Europe/London:20261109T100000\nSEQUENCE:0\nSTATUS:CANCELLED\nSUMMARY:Weekly review\n" + appleUid +
               "END:VEVENT\nEND:VCALENDAR\n");
            expect(Imip::ApplyToStored(apple, appleOne, guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused) && refused == 0 &&
                   shown(apple) == "20261019T090000Z|20261026T100000Z|20261102T100000Z|20261116T100000Z|20261123T100000Z|20261130T100000Z",
               "Apple's CANCEL of one occurrence did not take out that occurrence alone");

            const AnsiString awayUid = "UID:040000008200E00074C5B7101A82E00800000000F17B7F4D6C5FDC01000000000000000010000000AABBCCDDEEFF00112233445566778899\n";
            ICalComponent away = parsed(withCrlf("BEGIN:VCALENDAR\nPRODID:Microsoft Exchange Server 2010\nVERSION:2.0\nBEGIN:VEVENT\n" + exchangePeople +
               "RRULE:FREQ=WEEKLY;COUNT=4;INTERVAL=1;BYDAY=MO;WKST=MO\n" + awayUid + "SUMMARY;LANGUAGE=en-GB:Away\nDTSTART;VALUE=DATE:20261102\n"
               "DTEND;VALUE=DATE:20261103\nCLASS:PUBLIC\nDTSTAMP:20260928T080000Z\nTRANSP:TRANSPARENT\nSTATUS:CONFIRMED\nSEQUENCE:0\n"
               "X-MICROSOFT-CDO-ALLDAYEVENT:TRUE\nEND:VEVENT\nEND:VCALENDAR\n"));
            const ImipMessage awayOne = clientMessage("BEGIN:VCALENDAR\nMETHOD:CANCEL\nPRODID:Microsoft Exchange Server 2010\nVERSION:2.0\nBEGIN:VEVENT\n" +
               exchangePeople + "RECURRENCE-ID;VALUE=DATE:20261109\n" + awayUid + "SUMMARY;LANGUAGE=en-GB:Canceled: Away\nDTSTART;VALUE=DATE:20261109\n"
               "DTEND;VALUE=DATE:20261110\nDTSTAMP:20260929T080000Z\nSTATUS:CANCELLED\nSEQUENCE:1\nX-MICROSOFT-CDO-ALLDAYEVENT:TRUE\n"
               "END:VEVENT\nEND:VCALENDAR\n");
            expect(Imip::ApplyToStored(away, awayOne, guest, sentOnlyBy(olga), noDelegate, noAlias, noList, refused) && refused == 0 &&
                   shown(away) == "20261102T000000Z|20261116T000000Z|20261123T000000Z" && valueOf(away, "EXDATE") == "VALUE=DATE:20261109",
               "Exchange's CANCEL of one day of an all-day series did not take out that day alone");
         }
      }

      // REPLY, at the organiser: the stored event is the account's own.
      {
         ICalComponent tree = storedEvent(self);
         expect(!Imip::ApplyToStored(tree, deliveredMessage("REPLY", self, "ATTENDEE;PARTSTAT=DECLINED:mailto:" + guest + "\r\n"), self, nobody, noDelegate, noAlias, noList, refused) &&
            partstatOf(tree, guest) == "NEEDS-ACTION" && refused == 1, "a REPLY in a guest's name that nobody verified changed the guest's answer");

         const AnsiString coGuestLines = "ATTENDEE;PARTSTAT=DECLINED:mailto:" + guest + "\r\n" +
            "ATTENDEE;SENT-BY=\"mailto:" + coGuest + "\";PARTSTAT=DECLINED:mailto:" + guest + "\r\n" +
            "ATTENDEE;PARTSTAT=ACCEPTED:mailto:" + coGuest + "\r\n" +
            "ATTENDEE;PARTSTAT=ACCEPTED:mailto:stranger@elsewhere.example\r\n";
         expect(Imip::ApplyToStored(tree, deliveredMessage("REPLY", self, coGuestLines), self, sentOnlyBy(coGuest), noDelegate, noAlias, noList, refused),
            "a co-guest's own answer, verified, was not applied");
         expect(partstatOf(tree, guest) == "NEEDS-ACTION", "a co-guest answered for a guest, plainly or as a delegate it is not");
         expect(partstatOf(tree, coGuest) == "ACCEPTED", "the co-guest's own answer is not in the event");
         expect(partstatOf(tree, "stranger@elsewhere.example") == "(not listed)", "a REPLY added an attendee the event does not list");
         expect(refused == 3, "the answers left as they were were not counted");
         expect(!Imip::ApplyToStored(tree, deliveredMessage("REPLY", self, "ATTENDEE;PARTSTAT=ACCEPTED:mailto:stranger@elsewhere.example\r\n"), self,
                                     sentOnlyBy("stranger@elsewhere.example"), noDelegate, noAlias, noList, refused) &&
                partstatOf(tree, "stranger@elsewhere.example") == "(not listed)" && refused == 1,
            "a REPLY from an address the event does not list, sent by that address, added it to the event");

         const std::function<bool(const AnsiString &, const AnsiString &)> guestsDeputy = [&](const AnsiString &attendee, const AnsiString &delegate)
         {
            return Imip::AddressOf(attendee) == guest && Imip::AddressOf(delegate) == deputy;
         };
         expect(Imip::ApplyToStored(tree, deliveredMessage("REPLY", self, "ATTENDEE;SENT-BY=\"mailto:" + deputy + "\";PARTSTAT=ACCEPTED:mailto:" + guest + "\r\n"), self,
                                    sentOnlyBy(deputy), guestsDeputy, noAlias, noList, refused) &&
                partstatOf(tree, guest) == "ACCEPTED" && sentByOf(tree, guest) == deputy,
            "the answer of a guest's delegate, verified, was not applied with its SENT-BY");
         expect(Imip::ApplyToStored(tree, deliveredMessage("REPLY", self, "ATTENDEE;PARTSTAT=TENTATIVE:mailto:" + guest + "\r\n"), self, sentOnlyBy(guest), noDelegate, noAlias, noList, refused) &&
                partstatOf(tree, guest) == "TENTATIVE" && sentByOf(tree, guest).IsEmpty(),
            "the guest's own answer, verified, was not applied, or left the delegate's SENT-BY beside it");
      }
      {
         ICalComponent tree = storedEvent(olga);
         expect(!Imip::ApplyToStored(tree, deliveredMessage("REPLY", olga, "ATTENDEE;PARTSTAT=DECLINED:mailto:" + guest + "\r\n"), self, sentOnlyBy(guest), noDelegate, noAlias, noList, refused) &&
                partstatOf(tree, guest) == "NEEDS-ACTION",
            "a REPLY changed an event another account organises");
      }

      // An attendee the event lists by another address of the same mailbox -
      // the invitation went to an alias, the answer came from the account: the
      // answer is set on the address the event lists, and only when the sender
      // may send as that address, so it changes nothing that naming the
      // listed address would not have. Nothing is added.
      const AnsiString alias = "g.smith@example.com";
      auto eventListing = [&](const AnsiString &attendeeLines) -> ICalComponent
      {
         const AnsiString text = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//ImipTester//EN\r\nBEGIN:VEVENT\r\n" + uidLine +
            "DTSTAMP:20260928T080000Z\r\nDTSTART:20261026T090000Z\r\nDTEND:20261026T100000Z\r\nSUMMARY:Board\r\nSEQUENCE:0\r\n" +
            "ORGANIZER:mailto:" + self + "\r\n" + attendeeLines + "END:VEVENT\r\nEND:VCALENDAR\r\n";
         ICalComponent tree;
         AnsiString problem;
         if (!ICalendar::Parse(text, tree, problem))
            throw std::logic_error(std::string("ImipTester: the stored event did not parse: ") + problem.c_str());
         return tree;
      };
      // What SenderPermittedFor grants the guest's own session: the guest's
      // address and the alias that points at it.
      const std::function<bool(const AnsiString &)> guestOrAlias = [&](const AnsiString &address)
      {
         const AnsiString wanted = Imip::AddressOf(address);
         return wanted == guest || wanted == alias;
      };
      const std::function<bool(const AnsiString &, const AnsiString &)> guestsAlias = [&](const AnsiString &listed, const AnsiString &named)
      {
         const AnsiString one = Imip::AddressOf(listed);
         const AnsiString other = Imip::AddressOf(named);
         return (one == alias && other == guest) || (one == guest && other == alias);
      };
      const AnsiString guestAccepts = "ATTENDEE;PARTSTAT=ACCEPTED:mailto:" + guest + "\r\n";
      const AnsiString aliasListed = "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + alias + "\r\nATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + coGuest + "\r\n";
      {
         ICalComponent tree = eventListing(aliasListed);
         expect(Imip::ApplyToStored(tree, deliveredMessage("REPLY", self, guestAccepts), self, guestOrAlias, noDelegate, guestsAlias, noList, refused) &&
                partstatOf(tree, alias) == "ACCEPTED" && partstatOf(tree, guest) == "(not listed)" && refused == 0,
            "the answer of a guest invited by an alias, from the guest's own address, was not set on the alias the event lists, or added the address it came from");
      }
      {
         ICalComponent tree = eventListing(aliasListed);
         expect(!Imip::ApplyToStored(tree, deliveredMessage("REPLY", self, guestAccepts), self, guestOrAlias, noDelegate, noAlias, noList, refused) &&
                partstatOf(tree, alias) == "NEEDS-ACTION" && partstatOf(tree, guest) == "(not listed)" && refused == 1,
            "an answer from an address the event does not list, that is no mailbox the event lists, changed the event");
      }
      {
         ICalComponent tree = eventListing(aliasListed);
         expect(!Imip::ApplyToStored(tree, deliveredMessage("REPLY", self, guestAccepts), self, sentOnlyBy(coGuest), noDelegate, guestsAlias, noList, refused) &&
                partstatOf(tree, alias) == "NEEDS-ACTION" && partstatOf(tree, coGuest) == "NEEDS-ACTION" && refused == 1,
            "a co-guest's REPLY naming another address of a guest's mailbox changed the guest's answer");
      }
      {
         // Both addresses listed: the answer is for the one the REPLY names.
         ICalComponent tree = eventListing("ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + guest + "\r\n" + aliasListed);
         expect(Imip::ApplyToStored(tree, deliveredMessage("REPLY", self, guestAccepts), self, guestOrAlias, noDelegate, guestsAlias, noList, refused) &&
                partstatOf(tree, guest) == "ACCEPTED" && partstatOf(tree, alias) == "NEEDS-ACTION",
            "an answer from an address the event lists was set on another address of the same mailbox");
      }

      // An occurrence the organiser left a guest out of stays so when the
      // guest answers for the series.
      {
         const AnsiString text = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//ImipTester//EN\r\n"
            "BEGIN:VEVENT\r\n" + uidLine + "DTSTAMP:20260928T080000Z\r\nDTSTART:20261026T090000Z\r\nDTEND:20261026T100000Z\r\n"
            "RRULE:FREQ=WEEKLY;COUNT=4\r\nSUMMARY:Board\r\nSEQUENCE:0\r\nORGANIZER:mailto:" + self + "\r\n" +
            "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + guest + "\r\nATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + coGuest + "\r\nEND:VEVENT\r\n"
            "BEGIN:VEVENT\r\n" + uidLine + "RECURRENCE-ID:20261102T090000Z\r\nDTSTAMP:20260928T080000Z\r\nDTSTART:20261102T090000Z\r\n"
            "DTEND:20261102T100000Z\r\nSUMMARY:Board without the guest\r\nSEQUENCE:0\r\nORGANIZER:mailto:" + self + "\r\n" +
            "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + coGuest + "\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
         ICalComponent tree;
         AnsiString problem;
         if (!ICalendar::Parse(text, tree, problem))
            throw std::logic_error(std::string("ImipTester: the recurring event did not parse: ") + problem.c_str());
         auto occurrenceLists = [&](const AnsiString &address) -> bool
         {
            for (size_t c = 0; c < tree.children.size(); c++)
            {
               const ICalComponent &component = tree.children[c];
               if (component.name != "VEVENT" || !component.Property("RECURRENCE-ID"))
                  continue;
               for (size_t i = 0; i < component.properties.size(); i++)
               {
                  if (component.properties[i].name == "ATTENDEE" && Imip::AddressOf(component.properties[i].value) == address)
                     return true;
               }
            }
            return false;
         };
         expect(Imip::ApplyToStored(tree, deliveredMessage("REPLY", self, guestAccepts), self, sentOnlyBy(guest), noDelegate, noAlias, noList, refused) &&
                partstatOf(tree, guest) == "ACCEPTED" && !occurrenceLists(guest) && occurrenceLists(coGuest),
            "a guest's answer for the series added the guest to an occurrence the organiser had left them out of");
      }

      // A guest invited through a distribution list of this server's that the
      // event lists: their own answer is added beside the list, with MEMBER
      // naming it, and only on the components that list it; nobody else's
      // answer adds them, and nobody who is not in the list is added.
      {
         const AnsiString team = "team@example.com";
         const std::function<bool(const AnsiString &, const AnsiString &)> guestInTeam = [&](const AnsiString &list, const AnsiString &named)
         {
            return Imip::AddressOf(list) == team && Imip::AddressOf(named) == guest;
         };
         auto memberOf = [](const ICalComponent &tree, const AnsiString &address) -> AnsiString
         {
            const ICalComponent *master = MasterOf(tree);
            for (size_t i = 0; master && i < master->properties.size(); i++)
            {
               const ICalProperty &property = master->properties[i];
               if (property.name == "ATTENDEE" && Imip::AddressOf(property.value) == address)
                  return Imip::AddressOf(property.Parameter("MEMBER"));
            }
            return "";
         };
         const AnsiString text = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//ImipTester//EN\r\n"
            "BEGIN:VEVENT\r\n" + uidLine + "DTSTAMP:20260928T080000Z\r\nDTSTART:20261026T090000Z\r\nDTEND:20261026T100000Z\r\n"
            "RRULE:FREQ=WEEKLY;COUNT=4\r\nSUMMARY:Board\r\nSEQUENCE:0\r\nORGANIZER:mailto:" + self + "\r\n" +
            "ATTENDEE;CUTYPE=GROUP;PARTSTAT=NEEDS-ACTION:mailto:" + team + "\r\nATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + coGuest + "\r\nEND:VEVENT\r\n"
            "BEGIN:VEVENT\r\n" + uidLine + "RECURRENCE-ID:20261102T090000Z\r\nDTSTAMP:20260928T080000Z\r\nDTSTART:20261102T090000Z\r\n"
            "DTEND:20261102T100000Z\r\nSUMMARY:Board without the list\r\nSEQUENCE:0\r\nORGANIZER:mailto:" + self + "\r\n" +
            "ATTENDEE;PARTSTAT=NEEDS-ACTION:mailto:" + coGuest + "\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
         auto listEvent = [&]() -> ICalComponent
         {
            ICalComponent tree;
            AnsiString problem;
            if (!ICalendar::Parse(text, tree, problem))
               throw std::logic_error(std::string("ImipTester: the event with a list did not parse: ") + problem.c_str());
            return tree;
         };
         auto occurrenceLists = [](const ICalComponent &tree, const AnsiString &address) -> bool
         {
            for (size_t c = 0; c < tree.children.size(); c++)
            {
               const ICalComponent &component = tree.children[c];
               if (component.name != "VEVENT" || !component.Property("RECURRENCE-ID"))
                  continue;
               for (size_t i = 0; i < component.properties.size(); i++)
               {
                  if (component.properties[i].name == "ATTENDEE" && Imip::AddressOf(component.properties[i].value) == address)
                     return true;
               }
            }
            return false;
         };
         {
            ICalComponent tree = listEvent();
            expect(Imip::ApplyToStored(tree, deliveredMessage("REPLY", self, guestAccepts), self, sentOnlyBy(guest), noDelegate, noAlias, guestInTeam, refused) &&
                   partstatOf(tree, guest) == "ACCEPTED" && memberOf(tree, guest) == team && partstatOf(tree, team) == "NEEDS-ACTION" &&
                   !occurrenceLists(tree, guest) && refused == 0,
               "the answer of a guest invited through a list the event lists, sent by the guest, was not added beside the list with MEMBER, or was added where the list is not");
         }
         {
            ICalComponent tree = listEvent();
            expect(!Imip::ApplyToStored(tree, deliveredMessage("REPLY", self, guestAccepts), self, sentOnlyBy(coGuest), noDelegate, noAlias, guestInTeam, refused) &&
                   partstatOf(tree, guest) == "(not listed)" && refused == 1,
               "a co-guest's REPLY in the name of a member of the list added the member");
         }
         {
            ICalComponent tree = listEvent();
            expect(!Imip::ApplyToStored(tree, deliveredMessage("REPLY", self, guestAccepts), self, nobody, noDelegate, noAlias, guestInTeam, refused) &&
                   partstatOf(tree, guest) == "(not listed)" && refused == 1,
               "a REPLY in the name of a member of the list that nobody verified added the member");
         }
         {
            ICalComponent tree = listEvent();
            const AnsiString strangerAccepts = "ATTENDEE;PARTSTAT=ACCEPTED:mailto:stranger@elsewhere.example\r\n";
            expect(!Imip::ApplyToStored(tree, deliveredMessage("REPLY", self, strangerAccepts), self, sentOnlyBy("stranger@elsewhere.example"), noDelegate, noAlias, guestInTeam, refused) &&
                   partstatOf(tree, "stranger@elsewhere.example") == "(not listed)" && refused == 1,
               "the verified answer of somebody who is not in the list added them");
         }
      }

      // Where a message came from: the account a signed-in session submitted
      // it as, and never the account an application's API key sent as.
      {
         std::shared_ptr<Message> original = std::shared_ptr<Message>(new Message(false));
         original->SetSubmitterAccountID(42);
         original->SetAuthDomains(_T("dmarc=partner.example"));
         const ImipOrigin fromSession = Imip::OriginOf(original, String());
         if (fromSession.submitterAccountId != 42 || fromSession.authDomains != "dmarc=partner.example")
            throw std::logic_error("ImipTester: OriginOf did not carry the submitter and the DMARC record of the message.");
         // As the send API records one: no DMARC record, which would have
         // OriginOf read a delivered copy's header.
         original->SetAuthDomains(String());
         original->SetSubmittedThroughSendApi(true);
         const ImipOrigin fromKey = Imip::OriginOf(original, String());
         if (fromKey.submitterAccountId != 0)
            throw std::logic_error("ImipTester: OriginOf took a message sent through the send API for a session of the account its key sent as.");
      }

      // Whatever the origin says, an empty address is nobody's, and an origin
      // with nothing recorded vouches for no address from outside. (An
      // address of this server's own, and a signed-in session, need the
      // database; the regression suite's RestApiCalendar and
      // RestApiCalendarSharing hold them.)
      ImipOrigin nothing;
      if (Imip::IsSentBy(nothing, ""))
         throw std::logic_error("ImipTester: IsSentBy vouched for an empty address.");
      ImipOrigin fromOutside;
      fromOutside.authDomains = "dmarc=partner.example";
      fromOutside.fromAddress = "olga@partner.example";
      if (Imip::IsSentBy(fromOutside, "mailto:"))
         throw std::logic_error("ImipTester: IsSentBy vouched for an address with nothing in it.");
   }
}
