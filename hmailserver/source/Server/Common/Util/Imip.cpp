// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See Imip.h.

#include "StdAfx.h"

#include <algorithm>
#include <atomic>
#include <ctime>
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
      AnsiString sequence = Trimmed(master->Value("SEQUENCE"));
      out.sequence = sequence.IsEmpty() ? 0 : atoi(sequence.c_str());

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
      for (size_t i = 0; i < copy.properties.size(); i++)
      {
         if (copy.properties[i].name == "METHOD")
         {
            if (method.IsEmpty())
            {
               copy.properties.erase(copy.properties.begin() + i);
               i--;
               continue;
            }
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
            for (size_t p = 0; p < property.parameters.size();)
            {
               if (property.parameters[p].first == "PARTSTAT")
               {
                  property.parameters[p].second = partstat;
                  had = true;
               }
               else if (property.parameters[p].first == "RSVP")
                  property.parameters[p].second = "FALSE";
               else if (property.parameters[p].first == "SENT-BY")
               {
                  // Whoever answered last is who answered.
                  property.parameters.erase(property.parameters.begin() + p);
                  continue;
               }
               p++;
            }
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

      int SequenceOf(const ICalComponent &component)
      {
         AnsiString text = Trimmed(component.Value("SEQUENCE"));
         return text.IsEmpty() ? 0 : atoi(text.c_str());
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
            for (size_t i = 0; i < component.properties.size();)
            {
               if (component.properties[i].name == UnverifiedMark)
                  component.properties.erase(component.properties.begin() + i);
               else
                  i++;
            }
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
         MarkCancelled(tree);
         return true;
      }

      return false;
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
            // The stored event's organiser must be the invitation's. A UID is
            // mailed to every guest, so anybody who saw an invitation could
            // otherwise send a REQUEST under it, with themselves as organiser
            // and a higher SEQUENCE, and rewrite the event in the account's own
            // calendar on delivery. An event the account made itself has the
            // account, or nobody, as organiser and is refused the same way.
            if (storedMaster && AddressOf(storedMaster->Value("ORGANIZER")) != invitation.organizer)
            {
               problem = "the stored event has another organiser";
               return false;
            }
            if (storedMaster)
               storedSequence = SequenceOf(*storedMaster);
            storedUnverified = FiledUnverified(storedTree);
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
      // The stored copy is a plain object: the METHOD is the mail's.
      for (size_t i = 0; i < tree.properties.size();)
      {
         if (tree.properties[i].name == "METHOD")
            tree.properties.erase(tree.properties.begin() + i);
         else
            i++;
      }
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
