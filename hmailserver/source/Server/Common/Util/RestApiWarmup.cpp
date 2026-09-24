// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The IP warm-up as a resource. See RestApiServer.h and SMTP/IpWarmup.h.
//
//    /api/v1/warmup                 the schedules with today's counts, and a new one
//    /api/v1/warmup/{id}            one schedule, changed or removed
//    /api/v1/warmup/{id}/history    one schedule's counts, every day it has
//
// A schedule is an outbound address, the UTC day it began, the plan it follows
// and whether it is enabled; the listing adds what the queue did with it today,
// per provider - recipients sent, recipients deferred over the ceiling, and the
// ceiling itself - so that the Deck's queue view can show the warm-up beside
// the queue it shapes. The body of a write is read whole before anything is
// applied, so a wrong type, an unknown field or a refused value is a 400 naming
// it and nothing changes; a second schedule for an address is a 409, and so is
// the one of two concurrent POSTs that lost the race to the unique index. A
// schedule's address is what its counts are about, so a PUT does not change
// it: delete the schedule and add another.
//
// Any syntactically valid IPv4 or IPv6 address is accepted, the unspecified
// ones apart. Whether the server will ever bind it is not something a route can
// know: the address has to be configured on the host, and bound by
// SMTPDeliveryBindToIP or by a global rule's bind-to-address action, which
// overrides the setting, for a delivery to leave from it. The entry says
// whether the setting binds it for its own family, as a delivery reads the
// setting (in_bind_setting), and docs/IpWarmup.md says the rest.
//
// Server-wide and under the queue's permissions: a schedule decides how much
// the queue sends from an address every domain's mail may leave from.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "JsonDocument.h"
#include "Unicode.h"

#include "../../SMTP/IpWarmup.h"
#include "../../SMTP/OutboundProviders.h"
#include "../../SMTP/SMTPConfiguration.h"

#include <ctime>
#include <map>
#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace
{
   using namespace HM;

   // The two private helpers of RestApiServer these functions need, handed in by
   // the member function that owns the right to name them.
   struct Bridge
   {
      AnsiString (*escape)(const AnsiString &value);
      HttpResponse (*respond)(int statusCode, const AnsiString &body, const AnsiString &extraHeaders);
   };

   AnsiString Utf8(const String &value)
   {
      AnsiString utf8;
      Unicode::WideToMultiByte(value, utf8);
      return utf8;
   }

   String Utf8ToString(const std::string &utf8)
   {
      String value;
      Unicode::MultiByteToWide(AnsiString(utf8.c_str()), value);
      return value;
   }

   AnsiString Number(__int64 value)
   {
      AnsiString text;
      text.Format("%I64d", value);
      return text;
   }

   AnsiString Text(const Bridge &bridge, const String &value)
   {
      return "\"" + bridge.escape(Utf8(value)) + "\"";
   }

   HttpResponse Refusal(const Bridge &bridge, int status, const String &sentence)
   {
      return bridge.respond(status, "{\"error\":\"" + bridge.escape(Utf8(sentence)) + "\"}", "");
   }

   bool ParseObjectBody(const AnsiString &requestBody, JsonValue &body)
   {
      std::string parseError;
      std::string text(requestBody.c_str(), (size_t) requestBody.GetLength());
      return JsonValue::Parse(text, body, parseError) && body.IsObject();
   }

   __int64 Today()
   {
      return IpWarmup::DayOf((__int64) time(nullptr));
   }

   // Whether SMTPDeliveryBindToIP binds the address, read exactly as a
   // delivery reads it (SMTPConfiguration::OutboundBindAddress): the first
   // entry of the family of the host being reached - here the address's own
   // family - and nothing else. An address named second of its family is
   // never bound, and is answered false. A global rule's bind-to-address
   // action overrides the setting and is not read here; the doc says so.
   bool InBindSetting(const String &address)
   {
      IPAddress parsed;
      if (!parsed.TryParse(AnsiString(address), false))
         return false;

      const String setting = Configuration::Instance()->GetSMTPConfiguration()->GetSMTPDeliveryBindToIP();
      if (setting.IsEmpty())
         return false;

      const IPAddress bound = SMTPConfiguration::OutboundBindAddress(setting, parsed);
      if (bound.IsAny())
         return false;

      return String(bound.ToString()).CompareNoCase(address) == 0;
   }

   AnsiString CeilingJson(int ceiling)
   {
      return ceiling < 0 ? AnsiString("null") : Number(ceiling);
   }

   // One schedule as every route answers it: what it is, which day it is on,
   // and - from the counts handed in, which are today's - per provider what
   // was sent, what was deferred over the ceiling, and the ceiling (null when
   // the plan is over).
   AnsiString ScheduleJson(const Bridge &bridge, const IpWarmup::Schedule &schedule, __int64 today,
                           const std::vector<IpWarmup::Count> &counts)
   {
      const int day = IpWarmup::WarmupDay(schedule.started, today);
      const int lastDay = IpWarmup::LastDay(schedule.plan);

      AnsiString providers;

      for (int index = 0; index < OutboundProviders::Count; index++)
      {
         const OutboundProviders::Provider provider = (OutboundProviders::Provider) index;
         const String key = OutboundProviders::Key(provider);

         int sent = 0;
         int deferred = 0;

         for (const IpWarmup::Count &count : counts)
         {
            if (count.schedule_id == schedule.id && count.day == today && count.provider.CompareNoCase(key) == 0)
            {
               sent = count.sent;
               deferred = count.deferred;
            }
         }

         if (!providers.IsEmpty())
            providers += ",";

         providers += "{\"provider\":" + Text(bridge, key) +
                      ",\"sent\":" + Number(sent) +
                      ",\"deferred\":" + Number(deferred) +
                      ",\"ceiling\":" + CeilingJson(IpWarmup::Ceiling(schedule.plan, day, provider)) + "}";
      }

      return "{\"id\":" + Number(schedule.id) +
             ",\"address\":" + Text(bridge, schedule.address) +
             ",\"plan\":" + Text(bridge, IpWarmup::PlanName(schedule.plan)) +
             ",\"started\":" + Text(bridge, IpWarmup::FormatDate(schedule.started)) +
             ",\"enabled\":" + AnsiString(schedule.enabled ? "true" : "false") +
             ",\"day\":" + Number(day) +
             ",\"last_day\":" + Number(lastDay) +
             ",\"complete\":" + AnsiString(day > lastDay ? "true" : "false") +
             ",\"in_bind_setting\":" + AnsiString(InBindSetting(schedule.address) ? "true" : "false") +
             ",\"providers\":[" + providers + "]}";
   }

   bool UnknownKey(const JsonValue &object, String &error)
   {
      static const char *const known[] = { "address", "plan", "started", "enabled" };

      for (const std::pair<std::string, JsonValue> &member : object.Members())
      {
         bool found = false;
         for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
         {
            if (member.first == known[i])
            {
               found = true;
               break;
            }
         }

         if (!found)
         {
            error = String(_T("unknown field: ")) + Utf8ToString(member.first.substr(0, 48));
            return true;
         }
      }

      return false;
   }

   // The body over a schedule: a field left out keeps its value. False, with
   // the sentence, at the first field refused; the schedule is then half
   // written and the caller throws it away.
   bool ReadFields(const JsonValue &body, IpWarmup::Schedule &schedule, bool &addressGiven, String &error)
   {
      addressGiven = false;

      if (UnknownKey(body, error))
         return false;

      const JsonValue *address = body.Get("address");
      if (address && !address->IsNull())
      {
         String canonical;
         if (!address->IsString() || !IpWarmup::CanonicalAddress(Utf8ToString(address->AsString()), canonical))
         {
            error = _T("address must be an IPv4 or IPv6 address, and not 0.0.0.0 or ::");
            return false;
         }

         schedule.address = canonical;
         addressGiven = true;
      }

      const JsonValue *plan = body.Get("plan");
      if (plan && !plan->IsNull())
      {
         bool read = false;

         if (plan->IsString())
         {
            int named = 0;
            if (IpWarmup::PlanFromName(Utf8ToString(plan->AsString()), named))
            {
               schedule.plan = named;
               read = true;
            }
         }
         else if (plan->IsNumber())
         {
            for (int candidate = 0; candidate < IpWarmup::PlanCount; candidate++)
            {
               if (plan->AsNumber() == (double) candidate)
               {
                  schedule.plan = candidate;
                  read = true;
               }
            }
         }

         if (!read)
         {
            error = _T("plan must be standard, gentle or fast (or 0, 1 or 2)");
            return false;
         }
      }

      const JsonValue *started = body.Get("started");
      if (started && !started->IsNull())
      {
         __int64 day = 0;
         if (!started->IsString() || !IpWarmup::ParseDate(Utf8ToString(started->AsString()), day))
         {
            error = _T("started must be a date written YYYY-MM-DD");
            return false;
         }

         if (day > Today())
         {
            error = _T("started may not be later than today (UTC): a schedule counts from the day it begins");
            return false;
         }

         schedule.started = day;
      }

      const JsonValue *enabled = body.Get("enabled");
      if (enabled && !enabled->IsNull())
      {
         if (!enabled->IsBool())
         {
            error = _T("enabled must be true or false");
            return false;
         }

         schedule.enabled = enabled->AsBool();
      }

      return true;
   }

   HttpResponse Saved(const Bridge &bridge, int status, IpWarmup::Schedule &schedule)
   {
      bool taken = false;
      if (!IpWarmup::Instance()->AddressTaken(schedule.address, schedule.id, taken))
         return Refusal(bridge, 500, _T("the warm-up schedules could not be read; see the error log"));

      if (taken)
         return Refusal(bridge, 409, _T("a warm-up schedule for that address exists: change it (PUT), or delete it first"));

      if (!IpWarmup::Instance()->Save(schedule))
      {
         // Two POSTs for one address at once both pass the check above, and
         // the unique index refuses the second: that is the 409 it would have
         // had a moment later, not a failure of the server.
         bool takenNow = false;
         if (IpWarmup::Instance()->AddressTaken(schedule.address, schedule.id, takenNow) && takenNow)
            return Refusal(bridge, 409, _T("a warm-up schedule for that address exists: change it (PUT), or delete it first"));

         return Refusal(bridge, 500, _T("the warm-up schedule could not be saved; see the error log"));
      }

      LOG_APPLICATION("RestApi: IP warm-up schedule for " + schedule.address + " saved: " + IpWarmup::PlanName(schedule.plan) +
         ", from " + IpWarmup::FormatDate(schedule.started) + (schedule.enabled ? String(_T(", enabled.")) : String(_T(", disabled."))));

      const __int64 today = Today();
      std::vector<IpWarmup::Count> counts;
      IpWarmup::Instance()->CountsOn(today, counts);

      return bridge.respond(status, ScheduleJson(bridge, schedule, today, counts), "");
   }

   HttpResponse List(const Bridge &bridge)
   {
      std::vector<IpWarmup::Schedule> schedules;
      std::vector<IpWarmup::Count> counts;
      const __int64 today = Today();

      if (!IpWarmup::Instance()->ReadAll(schedules) || !IpWarmup::Instance()->CountsOn(today, counts))
         return Refusal(bridge, 500, _T("the warm-up schedules could not be read; see the error log"));

      AnsiString items;
      for (const IpWarmup::Schedule &schedule : schedules)
      {
         if (!items.IsEmpty())
            items += ",";

         items += ScheduleJson(bridge, schedule, today, counts);
      }

      return bridge.respond(200, "{\"today\":" + Text(bridge, IpWarmup::FormatDate(today)) + ",\"schedules\":[" + items + "]}", "");
   }

   HttpResponse Create(const Bridge &bridge, const AnsiString &requestBody)
   {
      JsonValue body;
      if (!ParseObjectBody(requestBody, body))
         return Refusal(bridge, 400, _T("the body must be a JSON object"));

      IpWarmup::Schedule schedule;
      schedule.started = Today();

      bool addressGiven = false;
      String error;
      if (!ReadFields(body, schedule, addressGiven, error))
         return Refusal(bridge, 400, error);

      if (!addressGiven)
         return Refusal(bridge, 400, _T("address is required: the outbound IPv4 or IPv6 address the schedule is for"));

      return Saved(bridge, 201, schedule);
   }

   HttpResponse Update(const Bridge &bridge, __int64 id, const AnsiString &requestBody)
   {
      JsonValue body;
      if (!ParseObjectBody(requestBody, body))
         return Refusal(bridge, 400, _T("the body must be a JSON object"));

      IpWarmup::Schedule schedule;
      bool found = false;
      if (!IpWarmup::Instance()->Read(id, schedule, found))
         return Refusal(bridge, 500, _T("the warm-up schedule could not be read; see the error log"));

      if (!found)
         return Refusal(bridge, 404, _T("warm-up schedule not found"));

      const String address = schedule.address;

      bool addressGiven = false;
      String error;
      if (!ReadFields(body, schedule, addressGiven, error))
         return Refusal(bridge, 400, error);

      // The counts are the schedule's, and they are about its address: moved
      // to another, they would say it had sent what it never did.
      if (addressGiven && schedule.address.CompareNoCase(address) != 0)
         return Refusal(bridge, 400, _T("a schedule's address cannot be changed, since its counts are about that address: delete the schedule and add one for the other address"));

      return Saved(bridge, 200, schedule);
   }

   HttpResponse Delete(const Bridge &bridge, __int64 id)
   {
      bool found = false;

      if (!IpWarmup::Instance()->Delete(id, found))
         return Refusal(bridge, 500, _T("the warm-up schedule could not be deleted; see the error log"));

      if (!found)
         return Refusal(bridge, 404, _T("warm-up schedule not found"));

      LOG_APPLICATION("RestApi: An IP warm-up schedule was deleted, with its counts.");

      return bridge.respond(200, "{\"deleted\":true}", "");
   }

   HttpResponse History(const Bridge &bridge, __int64 id)
   {
      IpWarmup::Schedule schedule;
      bool found = false;
      if (!IpWarmup::Instance()->Read(id, schedule, found))
         return Refusal(bridge, 500, _T("the warm-up schedule could not be read; see the error log"));

      if (!found)
         return Refusal(bridge, 404, _T("warm-up schedule not found"));

      std::vector<IpWarmup::Count> counts;
      if (!IpWarmup::Instance()->History(id, counts))
         return Refusal(bridge, 500, _T("the warm-up counts could not be read; see the error log"));

      AnsiString days;
      for (const IpWarmup::Count &count : counts)
      {
         // A row the day made and nothing came of - a reservation let go
         // whole, a refused connection - is not a day anything was counted.
         if (count.sent == 0 && count.deferred == 0)
            continue;

         if (!days.IsEmpty())
            days += ",";

         // The day and the ceiling as they were in force when the row was
         // written, stored with it: a plan or a start changed since does not
         // rewrite the past.
         days += "{\"date\":" + Text(bridge, IpWarmup::FormatDate(count.day)) +
                 ",\"day\":" + Number(count.plan_day) +
                 ",\"provider\":" + Text(bridge, count.provider) +
                 ",\"sent\":" + Number(count.sent) +
                 ",\"deferred\":" + Number(count.deferred) +
                 ",\"ceiling\":" + CeilingJson(count.ceiling) + "}";
      }

      return bridge.respond(200, "{\"id\":" + Number(schedule.id) +
                                 ",\"address\":" + Text(bridge, schedule.address) +
                                 ",\"plan\":" + Text(bridge, IpWarmup::PlanName(schedule.plan)) +
                                 ",\"started\":" + Text(bridge, IpWarmup::FormatDate(schedule.started)) +
                                 ",\"days\":[" + days + "]}", "");
   }
}

namespace HM
{
   AnsiString
   RestApiServer::OpenApiWarmupPaths_()
   {
      static const char *paths =
         ",\"/api/v1/warmup\":{"
         "\"get\":{\"summary\":\"The IP warm-up schedules, with what each has sent today\",\"description\":\"A schedule holds one outbound address to a daily ceiling of recipients per large provider, rising over the weeks of its plan (docs/IpWarmup.md). Answers today (the UTC date) and schedules, each with id, address, plan (standard, gentle or fast), started (the UTC date its day 1 was), enabled, day (which day of the warm-up today is), last_day (the plan's last day with a ceiling), complete (the plan is over and nothing is counted), in_bind_setting (whether SMTPDeliveryBindToIP binds the address as a delivery reads it: the first entry of the address's own family - a global rule's bind-to-address action overrides the setting and is not seen here, and a schedule counts every delivery that binds the address, from the setting or from a rule) and providers: for google, microsoft, yahoo, apple, proton and other, sent (recipients delivered today; not those a delivery still in progress holds), deferred (recipients held over the ceiling today, once per pass) and ceiling (null when there is none). The provider of a delivery is the one its first mail host belongs to, not the recipient's domain. Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"today and schedules\"}}},"
         "\"post\":{\"summary\":\"Add an IP warm-up schedule\",\"description\":\"Body: address (required; an IPv4 or IPv6 address, not 0.0.0.0 or ::, written back the way a delivery names it), plan (standard, gentle or fast, or 0, 1 or 2; standard by default), started (YYYY-MM-DD, the UTC date of day 1; today by default, and never later than today) and enabled (true by default); any other field is refused. Any valid address is accepted: it must also be configured on the host and bound by SMTPDeliveryBindToIP or a global rule's bind-to-address action, which overrides the setting, for a delivery to leave from it. In force for the next delivery. Server-wide; refused for domain-restricted and read-only keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"address\"],\"additionalProperties\":false,\"properties\":{\"address\":{\"type\":\"string\"},\"plan\":{\"oneOf\":[{\"type\":\"string\",\"enum\":[\"standard\",\"gentle\",\"fast\"]},{\"type\":\"integer\",\"enum\":[0,1,2]}]},\"started\":{\"type\":\"string\"},\"enabled\":{\"type\":\"boolean\"}}}}}},\"responses\":{\"201\":{\"description\":\"Created: the schedule as the listing shows it\"},\"400\":{\"description\":\"A field of the wrong type or value, or an unknown field (error names it)\"},\"409\":{\"description\":\"A schedule for that address exists\"}}}},"
         "\"/api/v1/warmup/{id}\":{"
         "\"put\":{\"summary\":\"Change an IP warm-up schedule\",\"description\":\"Body: any subset of plan, started and enabled; a field left out keeps its value, and any other field is refused. The address cannot be changed, since the counts are about it: an address other than the schedule's is a 400 - delete the schedule and add one. The counts are kept per UTC date with the ceiling that was in force, so a changed start or plan changes the ceilings from now and not what was counted. Nothing changes when the body is refused. Server-wide; refused for domain-restricted and read-only keys.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"address\":{\"type\":\"string\"},\"plan\":{\"oneOf\":[{\"type\":\"string\",\"enum\":[\"standard\",\"gentle\",\"fast\"]},{\"type\":\"integer\",\"enum\":[0,1,2]}]},\"started\":{\"type\":\"string\"},\"enabled\":{\"type\":\"boolean\"}}}}}},\"responses\":{\"200\":{\"description\":\"The schedule as saved\"},\"400\":{\"description\":\"A field refused, or an address other than the schedule's (error names it); nothing changed\"},\"404\":{\"description\":\"Unknown id\"}}},"
         "\"delete\":{\"summary\":\"Remove an IP warm-up schedule\",\"description\":\"The schedule and its counts go; the address is untouched by the warm-up from the next delivery. Mail already deferred by it waits until its next try and is then delivered as any other.\",\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"Unknown id\"}}}},"
         "\"/api/v1/warmup/{id}/history\":{"
         "\"get\":{\"summary\":\"One IP warm-up schedule's counts, every day it has\",\"description\":\"Newest first, one entry per UTC date and provider on which a recipient was delivered or held: date, day (the warm-up day in force that day), provider, sent, deferred and ceiling (the ceiling in force that day). The day and the ceiling are the ones stored with the count, not worked out again from the schedule as it is now. A date on which nothing was delivered to or held for a provider has no entry. Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"id, address, plan, started and days\"},\"404\":{\"description\":\"Unknown id\"}}}}";

      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleWarmup_(RouteKind kind, __int64 id, const AnsiString &requestBody)
   {
      Bridge bridge = { &RestApiServer::JsonEscape_, &RestApiServer::BuildResponse_ };

      switch (kind)
      {
      case RouteWarmupList:
         return List(bridge);
      case RouteWarmupCreate:
         return Create(bridge, requestBody);
      case RouteWarmupUpdate:
         return Update(bridge, id, requestBody);
      case RouteWarmupDelete:
         return Delete(bridge, id);
      case RouteWarmupHistory:
         return History(bridge, id);
      default:
         break;
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }
}
