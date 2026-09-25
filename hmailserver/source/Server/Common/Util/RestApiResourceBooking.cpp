// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Rooms and equipment over REST (ResourceBooking.h, docs/RoomsAndEquipment.md):
//
//    GET  /api/v1/accounts/{address}/booking-policy   the policy and the delegates
//    PUT  /api/v1/accounts/{address}/booking-policy   any subset of them
//
// What the account is - person, room or equipment - is the account's own
// field, kind, on GET and PUT /api/v1/accounts/{address}; this route is the
// policy a room or a piece of equipment answers by, and who answers for it
// when it does not. A person's policy is read (the defaults) and never
// written: the PUT answers 409 until the account is a room or a piece of
// equipment. A credential confined to named domains is scoped by the address
// in the path, as every account route is, and names delegates in those
// domains only (ResourceBooking::ResolveDelegates).

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "JsonDocument.h"
#include "ResourceBooking.h"
#include "Unicode.h"
#include "../Application/Logger.h"
#include "../Cache/CacheContainer.h"
#include "../BO/Account.h"

#include <cctype>
#include <cmath>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const char *DayWords[] = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" };

      String BookingWide(const std::string &utf8)
      {
         String out;
         Unicode::MultiByteToWide(AnsiString(utf8.c_str()), out);
         return out;
      }

      AnsiString ClockOf(int minutes)
      {
         AnsiString text;
         text.Format("%02d:%02d", minutes / 60, minutes % 60);
         return text;
      }

      // "HH:MM", 00:00 to 24:00, to minutes after midnight.
      bool ReadClock(const std::string &text, int &minutes)
      {
         if (text.size() != 5 || text[2] != ':')
            return false;
         for (int i = 0; i < 5; i++)
         {
            if (i != 2 && (text[i] < '0' || text[i] > '9'))
               return false;
         }
         const int hours = (text[0] - '0') * 10 + (text[1] - '0');
         const int mins = (text[3] - '0') * 10 + (text[4] - '0');
         if (hours > 24 || mins > 59 || (hours == 24 && mins != 0))
            return false;
         minutes = hours * 60 + mins;
         return true;
      }

      // A member that is a list of names: an array of strings, or one string
      // of them separated by commas or line breaks, as a form's text area
      // sends it.
      bool ReadNames(const JsonValue &member, std::vector<String> &names)
      {
         names.clear();
         if (member.IsNull())
            return true;
         if (member.IsString())
         {
            names = ResourceBooking::SplitList(BookingWide(member.AsString()));
            return true;
         }
         if (!member.IsArray())
            return false;
         for (size_t i = 0; i < member.Items().size(); i++)
         {
            const JsonValue &item = member.Items()[i];
            if (!item.IsString())
               return false;
            String name = BookingWide(item.AsString());
            name.TrimLeft();
            name.TrimRight();
            if (!name.IsEmpty())
               names.push_back(name);
         }
         return true;
      }

      bool ReadWholeNumber(const JsonValue &member, long minimum, long maximum, int &out)
      {
         if (!member.IsNumber())
            return false;
         const double number = member.AsNumber();
         if (std::floor(number) != number || number < (double) minimum || number > (double) maximum)
            return false;
         out = (int) number;
         return true;
      }
   }

   AnsiString
   RestApiServer::OpenApiResourceBookingPaths_()
   {
      static const char *paths =
         ",\"/api/v1/accounts/{address}/booking-policy\":{\"get\":{\"summary\":\"A room's or a piece of equipment's booking policy, and its delegates\",\"description\":\"{address,kind,stored,auto_accept,window_days,max_duration_minutes,working_hours_only,work_days,work_start,work_end,time_zone,allow_conflicts,allow_recurring,bookers,capacity,delegates}. kind is the account's (person, room or equipment; it is set on PUT /api/v1/accounts/{address}); stored is false while the account has no policy of its own and answers by the defaults - auto_accept true, window_days 180, max_duration_minutes 1440, working_hours_only false, work_days mon to fri, work_start 08:00, work_end 17:00, time_zone UTC, allow_conflicts false, allow_recurring true, bookers empty (everyone in this server's own domains), capacity 0 (not said). delegates are the accounts and groups the resource's calendar is shared with as a delegate: they see its bookings and answer for it. A person's policy is read and means nothing. Scoped to the address's domain.\",\"responses\":{\"200\":{\"description\":\"The policy\"},\"404\":{\"description\":\"No such account\"}}},"
         "\"put\":{\"summary\":\"Set a room's or a piece of equipment's booking policy, and its delegates\",\"description\":\"Any subset of auto_accept (answer invitations by the policy; false leaves every invitation for the delegates, who are told by mail; nothing the room has not decided is put into its calendar), window_days (1 to 1080: how many days ahead a booking may start), max_duration_minutes (0 for no limit, else at most 527040), working_hours_only, work_days (an array of mon, tue, wed, thu, fri, sat and sun), work_start and work_end (HH:MM, work_end after work_start and at most 24:00), time_zone (a zone this server knows: Europe/London, America/New_York, UTC), allow_conflicts, allow_recurring (a series is checked occurrence by occurrence, and must end inside the booking window), bookers (who may book: an array of addresses, @domains, distribution list addresses and group names, alias domains resolved; empty is everyone in this server's own domains. An organiser from outside is answered only when named here by address or @domain and the invitation passed DMARC for that domain (SPF alone vouches for nobody); one of this server's own only when a signed-in session sent it as that account or as one allowed to send as it. A group is checked when it is added, and one deleted since matches nobody; a credential confined to named domains names addresses and @domains only, and is refused a new group with 403), capacity (0 to 100000 people, published in the directory) and delegates (an array of this server's accounts' addresses and group names; the resource's calendar is shared with each as a delegate, and a delegate not named any more loses the share - a plain read or write share is left alone. A credential confined to named domains names accounts of those domains only, and no groups). A field the body does not name is left as it is; an unknown field is refused by name. The policy is saved first and the delegates after it: a 500 says which was not written. 409 while the account is a person: set kind to room or equipment on PUT /api/v1/accounts/{address} first. Scoped to the address's domain.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"auto_accept\":{\"type\":\"boolean\"},\"window_days\":{\"type\":\"integer\"},\"max_duration_minutes\":{\"type\":\"integer\"},\"working_hours_only\":{\"type\":\"boolean\"},\"work_days\":{\"type\":\"array\",\"items\":{\"type\":\"string\",\"enum\":[\"mon\",\"tue\",\"wed\",\"thu\",\"fri\",\"sat\",\"sun\"]}},\"work_start\":{\"type\":\"string\"},\"work_end\":{\"type\":\"string\"},\"time_zone\":{\"type\":\"string\"},\"allow_conflicts\":{\"type\":\"boolean\"},\"allow_recurring\":{\"type\":\"boolean\"},\"bookers\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"capacity\":{\"type\":\"integer\"},\"delegates\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}}}},\"responses\":{\"200\":{\"description\":\"The policy as saved\"},\"400\":{\"description\":\"A field of the wrong type or out of range, an unknown field, a time zone this server does not know, a booker that is neither an address, an @domain nor a group, or a delegate that is neither an account nor a group of this server (the reason is in error)\"},\"403\":{\"description\":\"A delegate outside the domains the credential is confined to, or a group named as a delegate or a booker by such a credential\"},\"404\":{\"description\":\"No such account\"},\"409\":{\"description\":\"The account is a person\"},\"500\":{\"description\":\"The policy could not be written (nothing was changed), or it was and the delegates could not all be (the error says so)\"}}}}";
      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleAccountBookingPolicy_(const Caller &caller, const String &address, RouteKind kind, const AnsiString &requestBody)
   {
      auto quote = [](const String &value) { return JsonEscape_(Utf8_(value)); };

      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(address);
      if (!account)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      BookingPolicy policy;
      if (!ResourceBooking::GetPolicy(account->GetID(), policy))
         return BuildResponse_(500, "{\"error\":\"the booking policy could not be read\"}");
      const String storedBookers = policy.bookers;

      if (kind == RouteAccountBookingPolicyPut)
      {
         if (!account->IsResource())
         {
            return BuildResponse_(409, "{\"error\":\"" + quote(account->GetAddress()) +
               " is a person, not a room or a piece of equipment; set its kind on PUT /api/v1/accounts/{address} first\"}");
         }

         JsonValue body;
         std::string parseError;
         std::string text(requestBody.c_str(), (size_t) requestBody.GetLength());
         if (!JsonValue::Parse(text, body, parseError) || !body.IsObject())
            return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

         static const char *const known[] =
         {
            "auto_accept", "window_days", "max_duration_minutes", "working_hours_only", "work_days", "work_start",
            "work_end", "time_zone", "allow_conflicts", "allow_recurring", "bookers", "capacity", "delegates"
         };

         if (body.Members().empty())
            return BuildResponse_(400, "{\"error\":\"nothing to update\"}");

         for (size_t i = 0; i < body.Members().size(); i++)
         {
            const std::string &key = body.Members()[i].first;
            bool recognised = false;
            for (size_t k = 0; !recognised && k < sizeof(known) / sizeof(known[0]); k++)
               recognised = key == known[k];
            if (!recognised)
               return BuildResponse_(400, "{\"error\":\"unknown field: " + quote(BookingWide(key.substr(0, 48))) + "\"}");
         }

         // The booleans.
         struct Flag
         {
            const char *key;
            bool *value;
         };
         const Flag flags[] =
         {
            { "auto_accept", &policy.autoAccept },
            { "working_hours_only", &policy.workingHoursOnly },
            { "allow_conflicts", &policy.allowConflicts },
            { "allow_recurring", &policy.allowRecurring },
         };
         for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
         {
            const JsonValue *member = body.Get(flags[i].key);
            if (!member)
               continue;
            if (!member->IsBool())
               return BuildResponse_(400, "{\"error\":\"" + AnsiString(flags[i].key) + " must be true or false\"}");
            *flags[i].value = member->AsBool();
         }

         // The numbers, each read in its range here and judged again with the
         // rest by CheckPolicy.
         struct Number
         {
            const char *key;
            int *value;
            long minimum;
            long maximum;
         };
         const Number numbers[] =
         {
            { "window_days", &policy.windowDays, 1, ResourceBooking::MaximumWindowDays },
            { "max_duration_minutes", &policy.maxMinutes, 0, ResourceBooking::MaximumDurationMinutes },
            { "capacity", &policy.capacity, 0, ResourceBooking::MaximumCapacity },
         };
         for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++)
         {
            const JsonValue *member = body.Get(numbers[i].key);
            if (!member)
               continue;
            if (!ReadWholeNumber(*member, numbers[i].minimum, numbers[i].maximum, *numbers[i].value))
            {
               AnsiString sentence;
               sentence.Format("{\"error\":\"%s must be a whole number from %ld to %ld\"}", numbers[i].key, numbers[i].minimum, numbers[i].maximum);
               return BuildResponse_(400, sentence);
            }
         }

         if (const JsonValue *member = body.Get("work_days"))
         {
            if (!member->IsArray())
               return BuildResponse_(400, "{\"error\":\"work_days must be an array of mon, tue, wed, thu, fri, sat and sun\"}");
            int days = 0;
            for (size_t i = 0; i < member->Items().size(); i++)
            {
               const JsonValue &item = member->Items()[i];
               bool found = false;
               if (item.IsString())
               {
                  std::string word = item.AsString();
                  for (size_t c = 0; c < word.size(); c++)
                     word[c] = (char) tolower((unsigned char) word[c]);
                  for (int d = 0; d < 7 && !found; d++)
                  {
                     if (word == DayWords[d])
                     {
                        days |= (1 << d);
                        found = true;
                     }
                  }
               }
               if (!found)
                  return BuildResponse_(400, "{\"error\":\"work_days must be an array of mon, tue, wed, thu, fri, sat and sun\"}");
            }
            policy.workDays = days;
         }

         const char *clocks[] = { "work_start", "work_end" };
         for (int c = 0; c < 2; c++)
         {
            const JsonValue *member = body.Get(clocks[c]);
            if (!member)
               continue;
            int minutes = 0;
            if (!member->IsString() || !ReadClock(member->AsString(), minutes))
               return BuildResponse_(400, "{\"error\":\"" + AnsiString(clocks[c]) + " must be a time of day as HH:MM\"}");
            if (c == 0)
               policy.workStart = minutes;
            else
               policy.workEnd = minutes;
         }

         if (const JsonValue *member = body.Get("time_zone"))
         {
            if (!member->IsString())
               return BuildResponse_(400, "{\"error\":\"time_zone must be a string\"}");
            policy.timeZone = BookingWide(member->AsString());
         }

         if (const JsonValue *member = body.Get("bookers"))
         {
            std::vector<String> bookers;
            if (!ReadNames(*member, bookers))
               return BuildResponse_(400, "{\"error\":\"bookers must be an array of addresses, @domains and group names\"}");
            policy.bookers = ResourceBooking::JoinList(bookers);
         }

         const JsonValue *delegatesMember = body.Get("delegates");
         std::vector<String> delegates;
         if (delegatesMember && !ReadNames(*delegatesMember, delegates))
            return BuildResponse_(400, "{\"error\":\"delegates must be an array of addresses and group names\"}");

         policy.accountId = account->GetID();

         String problem;
         bool confined = false;
         if (!ResourceBooking::CheckPolicy(policy, storedBookers, caller.domains.empty(), problem, confined))
            return BuildResponse_(confined ? 403 : 400, "{\"error\":\"" + quote(problem) + "\"}");

         // Every delegate resolved and checked before anything is written, so
         // a list with a name that is nobody changes nothing. A credential
         // confined to named domains names accounts of those domains only, and
         // is told so before an address outside them is looked up.
         std::vector<__int64> delegateAccounts;
         std::vector<__int64> delegateGroups;
         if (delegatesMember)
         {
            bool outside = false;
            if (!ResourceBooking::ResolveDelegates(account, delegates, caller.domains, delegateAccounts, delegateGroups, problem, outside))
               return BuildResponse_(outside ? 403 : 400, "{\"error\":\"" + quote(problem) + "\"}");
         }

         // The policy, then the delegates: a policy that cannot be saved has
         // changed nothing, and delegates that cannot all be written after it
         // are said to be so, beside the policy that was.
         if (!ResourceBooking::SavePolicy(policy))
            return BuildResponse_(500, "{\"error\":\"the booking policy could not be saved, and nothing was changed\"}");

         if (delegatesMember && !ResourceBooking::ApplyDelegates(account, delegateAccounts, delegateGroups, problem))
         {
            LOG_APPLICATION("REST API: the booking policy of " + account->GetAddress() + " was saved by " + String(caller.identity) + ", and its delegates were not: " + problem);
            return BuildResponse_(500, "{\"error\":\"the booking policy was saved, and the delegates were not: " + quote(problem) + "\"}");
         }

         String changed;
         for (size_t i = 0; i < body.Members().size(); i++)
         {
            if (!changed.IsEmpty())
               changed += _T(", ");
            changed += BookingWide(body.Members()[i].first);
         }
         LOG_APPLICATION("REST API: the booking policy of " + account->GetAddress() + " was changed by " + String(caller.identity) + " (" + changed + ").");

         if (!ResourceBooking::GetPolicy(account->GetID(), policy))
            return BuildResponse_(500, "{\"error\":\"the booking policy was saved and could not be read back\"}");
      }

      std::vector<String> delegateEntries;
      if (!ResourceBooking::DelegateEntries(account->GetID(), delegateEntries))
         return BuildResponse_(500, "{\"error\":\"the delegates could not be read\"}");

      AnsiString days;
      for (int d = 0; d < 7; d++)
      {
         if ((policy.workDays & (1 << d)) == 0)
            continue;
         days += (days.IsEmpty() ? "\"" : ",\"") + AnsiString(DayWords[d]) + "\"";
      }

      std::vector<String> bookerEntries = ResourceBooking::SplitList(policy.bookers);
      AnsiString bookers;
      for (size_t i = 0; i < bookerEntries.size(); i++)
         bookers += (i > 0 ? ",\"" : "\"") + quote(bookerEntries[i]) + "\"";

      AnsiString delegatesJson;
      for (size_t i = 0; i < delegateEntries.size(); i++)
         delegatesJson += (i > 0 ? ",\"" : "\"") + quote(delegateEntries[i]) + "\"";

      AnsiString numbersJson;
      numbersJson.Format(",\"window_days\":%d,\"max_duration_minutes\":%d", policy.windowDays, policy.maxMinutes);

      AnsiString capacityJson;
      capacityJson.Format(",\"capacity\":%d", policy.capacity);

      AnsiString json = "{\"address\":\"" + quote(account->GetAddress()) +
         "\",\"kind\":\"" + ResourceBooking::KindWord(account->GetKind()) +
         "\",\"stored\":" + (policy.stored ? "true" : "false") +
         ",\"auto_accept\":" + (policy.autoAccept ? "true" : "false") +
         numbersJson +
         ",\"working_hours_only\":" + (policy.workingHoursOnly ? "true" : "false") +
         ",\"work_days\":[" + days + "]" +
         ",\"work_start\":\"" + ClockOf(policy.workStart) +
         "\",\"work_end\":\"" + ClockOf(policy.workEnd) +
         "\",\"time_zone\":\"" + quote(policy.timeZone) +
         "\",\"allow_conflicts\":" + (policy.allowConflicts ? "true" : "false") +
         ",\"allow_recurring\":" + (policy.allowRecurring ? "true" : "false") +
         ",\"bookers\":[" + bookers + "]" +
         capacityJson +
         ",\"delegates\":[" + delegatesJson + "]}";

      return BuildResponse_(200, json);
   }
}
