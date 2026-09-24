// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See Imip.h.

#include "StdAfx.h"

#include <ctime>

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
#include "../BO/Account.h"
#include "../BO/Alias.h"
#include "../BO/DomainAliases.h"
#include "../Application/ObjectCache.h"
#include "../Cache/CacheContainer.h"
#include "../Mime/Mime.h"
#include "../Mime/MimeCode.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../Application/Logger.h"

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
   Imip::SetPartstat(ICalComponent &calendar, const AnsiString &address, const AnsiString &name, const AnsiString &partstat, const AnsiString &sentBy)
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

         if (!found)
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
   Imip::ReplyObject(const ImipMessage &invitation, const AnsiString &address, const AnsiString &name, const AnsiString &partstat, const AnsiString &sentBy)
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

      // Whether one of the invitation's attendees is the account: its own
      // address, an alias domain's spelling of it, or an account alias
      // (hm_aliases, one step) that points at it - what RecipientParser
      // resolves on the way in, read from the same caches. me is the account's
      // address, lower-cased.
      bool NamesAccount(const ImipMessage &message, const AnsiString &me)
      {
         std::shared_ptr<DomainAliases> domainAliases = ObjectCache::Instance()->GetDomainAliases();
         for (size_t i = 0; i < message.attendees.size(); i++)
         {
            String address;
            Unicode::MultiByteToWide(message.attendees[i].address, address);
            String primary = domainAliases ? domainAliases->ApplyAliasesOnAddress(address) : address;
            primary.ToLower();
            if (Utf8Of(primary) == me)
               return true;

            std::shared_ptr<const Alias> alias = CacheContainer::Instance()->GetAlias(primary);
            if (alias && alias->GetIsActive())
            {
               String value = alias->GetValue();
               value = domainAliases ? domainAliases->ApplyAliasesOnAddress(value) : value;
               value.ToLower();
               if (Utf8Of(value) == me)
                  return true;
            }
         }
         return false;
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
   }

   bool
   Imip::KeepInvitation(__int64 accountId, const ImipMessage &invitation, bool createDefault, bool cancelled,
                        const AnsiString &address, const AnsiString &name, const AnsiString &partstat,
                        CalendarRecord &calendar, CalendarObjectRecord &record, AnsiString &problem)
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
      // invitation does not undo a newer.
      bool useInvitation = true;
      ICalComponent storedTree;
      AnsiString keptPartstat;
      AnsiString keptSentBy;
      if (inCalendar)
      {
         AnsiString ignored;
         if (ICalendar::Parse(Utf8Of(stored.data), storedTree, ignored))
         {
            const ICalComponent *storedMaster = MasterOf(storedTree);
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
            if (storedMaster && SequenceOf(*storedMaster) > invitation.sequence)
               useInvitation = false;
            // A resend at the same SEQUENCE carries NEEDS-ACTION for the
            // account - the organiser never received the answer, or sent the
            // original again - and RFC 5546 steps the sequence for neither a
            // description nor an attendee-list change; the answer already given
            // is carried into the replacement.
            if (storedMaster && useInvitation && SequenceOf(*storedMaster) == invitation.sequence && !address.IsEmpty())
               ReadPartstat(*storedMaster, address, keptPartstat, keptSentBy);
         }
         else
            storedTree = invitation.tree;
      }
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
   Imip::FileOnDelivery(std::shared_ptr<const Account> account, const String &fileName, bool spam)
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

         if (message.method == "REQUEST")
         {
            // An invitation, kept as it arrived in the default calendar of an
            // account that does not organise the event, so that the page shows
            // it as tentative and a delegate can answer it; an account with no
            // calendar row gets nothing, since a delivery is not a use.
            String me = account->GetAddress();
            me.ToLower();
            AnsiString meUtf8 = Utf8Of(me);
            if (!message.organizer.IsEmpty() && message.organizer == meUtf8)
               return;
            // Only an invitation that names the account is the account's to
            // keep: a forwarded one, one to a list the account is in, and
            // anything sent to see what sticks name somebody else, or nobody.
            if (!NamesAccount(message, meUtf8))
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
            CalendarObjectRecord record;
            if (KeepInvitation(account->GetID(), message, false, false, meUtf8, Trimmed(Utf8Of(personName)), "", calendar, record, problem))
            {
               LOG_DEBUG("iMIP: a REQUEST for " + uid + " was filed into the calendar of " + account->GetAddress() + ".");
            }
            else
            {
               LOG_DEBUG("iMIP: a REQUEST for " + uid + " was not filed into the calendar of " + account->GetAddress() + ": " + String(problem));
            }
            return;
         }

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

            if (message.method == "REPLY")
            {
               // The reply is the organiser's to file: only an event this
               // account organises takes an attendee's answer.
               const ICalComponent *master = MasterOf(tree);
               AnsiString organizer = master ? AddressOf(master->Value("ORGANIZER")) : AnsiString("");
               String me = account->GetAddress();
               me.ToLower();
               if (!organizer.IsEmpty() && organizer != Utf8Of(me))
                  continue;
               for (size_t i = 0; i < message.attendees.size(); i++)
                  SetPartstat(tree, message.attendees[i].address, message.attendees[i].name, message.attendees[i].partstat);
            }
            else
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
         // Delivery has happened; nothing here may undo it.
      }
   }
}
