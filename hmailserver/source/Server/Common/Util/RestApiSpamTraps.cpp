// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Spam traps as a resource, and the record of what has addressed them. See
// RestApiServer.h and Common/AntiSpam/SpamTrapMonitor.h.
//
//    /api/v1/domains/{domain}/spam-traps        the domain's traps
//    /api/v1/spam-traps/{address}               one trap, removed
//    /api/v1/domains/{domain}/spam-traps/hits   what has addressed this domain's
//    /api/v1/spam-traps/hits                    what has addressed any, and the clear
//
// A trap is listed and created under its domain and removed at its own address,
// which is the shape an alias and a distribution list already have: the address
// is unique in itself, and the domain it belongs to is the one in the address, so
// a credential confined to named domains is scoped by exactly the same rule
// (DomainNamedByRoute_) rather than by a second reading of the path.
//
// The hits are a record of traffic rather than configuration, so they are not on
// the audit trail and the clear says how many rows it removed. What the hits made
// - a ban, a blocked sender - outlives the record, and the clear does not undo it.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "JsonDocument.h"
#include "Unicode.h"

#include "../AntiSpam/SpamTrapMonitor.h"
#include "../BO/Domain.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentSpamTrap.h"
#include "../Util/AuditTrail.h"
#include "../Util/Parsing/StringParser.h"
#include "../Util/Time.h"
#include "../Util/VariantDateTime.h"

#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace
{
   using namespace HM;

   const int DefaultHitRows = 100;

   bool ParseObjectBody(const AnsiString &requestBody, JsonValue &body)
   {
      std::string parseError;
      std::string text(requestBody.c_str(), (size_t) requestBody.GetLength());
      return JsonValue::Parse(text, body, parseError) && body.IsObject();
   }

   int LastAt(const String &address)
   {
      for (int i = address.GetLength() - 1; i >= 0; i--)
      {
         if (address[i] == '@')
            return i;
      }

      return -1;
   }
}

namespace HM
{
   AnsiString
   RestApiServer::OpenApiSpamTrapPaths_()
   {
      // Begins with a comma and does not end with one, as every piece
      // HandleOpenApi_ appends does.
      return AnsiString(
         ",\"/api/v1/domains/{domain}/spam-traps\":{"
         "\"get\":{\"summary\":\"The domain's spam traps\",\"description\":\"Addresses in this domain that were never real. A message to one is refused exactly as a message to any address this server does not have - the sender is never told a trap exists - and then the connecting address is banned for spam_trap_ban_minutes, the envelope sender is added to the server's block list for spam_trap_block_days, and the hit is recorded. An authenticated session, a trusted IP range, the anti-spam white list, an incoming relay and AutoBanNeverBan are never banned. Each entry: id, address, local_part, created.\",\"responses\":{\"200\":{\"description\":\"domain, spam_traps\"},\"404\":{\"description\":\"No such domain\"}}},"
         "\"post\":{\"summary\":\"Declare an address a spam trap\",\"description\":\"Body: address - a whole address in this domain, or just the part before the @, which is taken to be in it. An address that is already an account, an alias or a distribution list is refused with 409, and so is an account, alias or list later given a trap's address. An address that is already a trap answers 200 with the entry it already had rather than making a second one.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"address\"],\"properties\":{\"address\":{\"type\":\"string\"}}}}}},\"responses\":{\"201\":{\"description\":\"The trap\"},\"200\":{\"description\":\"The trap, which was already one\"},\"400\":{\"description\":\"An address this domain cannot trap, with the reason in error\"},\"404\":{\"description\":\"No such domain\"},\"409\":{\"description\":\"An account, an alias or a distribution list has that address\"}}}},"
         "\"/api/v1/spam-traps/{address}\":{"
         "\"delete\":{\"summary\":\"Stop trapping an address\",\"description\":\"The address stops being a trap at once. What its hits already did - a banned address, a blocked sender - stays until it expires or is removed; the record of the hits stays too. Scoped to the domain in the address.\",\"responses\":{\"200\":{\"description\":\"Removed\"},\"404\":{\"description\":\"No such spam trap\"}}}},"
         "\"/api/v1/domains/{domain}/spam-traps/hits\":{"
         "\"get\":{\"summary\":\"What has addressed this domain's spam traps\",\"description\":\"Newest first, at most limit rows (1 to 1000, 100 by default). Each entry: id, time, address, domain, ip_address, sender, banned, blocked and note - which says what was done, or why the address was not banned or the sender not blocked. Kept for spam_trap_hit_retention_days.\",\"responses\":{\"200\":{\"description\":\"items\"},\"404\":{\"description\":\"No such domain\"}}}},"
         "\"/api/v1/spam-traps/hits\":{"
         "\"get\":{\"summary\":\"What has addressed any spam trap\",\"description\":\"The same entries as a domain's, for every domain, with hits_since_start - the counter the metrics exposition publishes as hmailserver_spam_trap_hits_total. Server-wide; refused for domain-restricted keys.\",\"responses\":{\"200\":{\"description\":\"items, hits_since_start\"}}},"
         "\"delete\":{\"summary\":\"Clear the record of spam trap hits\",\"description\":\"Removes every recorded hit and answers how many. The bans and the blocked senders they made are untouched, and the counter is not reset - this clears a record, it does not undo anything. Server-wide; refused for domain-restricted and read-only keys. On the audit trail as object type spam trap hits, action cleared, since the rows themselves are traffic and go unrecorded.\",\"responses\":{\"200\":{\"description\":\"removed\"},\"500\":{\"description\":\"The record could not be cleared\"}}}}");
   }

   HttpResponse
   RestApiServer::HandleSpamTraps_(const Route &route, const Caller &caller, const AnsiString &requestBody)
   {
      switch (route.kind)
      {
      case RouteSpamTrapHits:
      case RouteSpamTrapDomainHits:
         {
            __int64 domainId = 0;

            if (route.kind == RouteSpamTrapDomainHits)
            {
               std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(String(route.identifier));

               if (!domain)
                  return BuildResponse_(404, "{\"error\":\"no such domain\"}");

               domainId = domain->GetID();
            }

            int rows = DefaultHitRows;

            const AnsiString limit = QueryParameter_(route.query, "limit");

            if (!limit.IsEmpty())
            {
               rows = atoi(limit.c_str());

               if (rows < 1 || rows > 1000)
                  return BuildResponse_(400, "{\"error\":\"limit is a number between 1 and 1000\"}");
            }

            bool readable = false;
            const AnsiString document = SpamTrapMonitor::HitsDocument(domainId, rows, readable);

            if (!readable)
               return BuildResponse_(503, "{\"error\":\"the record of spam trap hits could not be read\"}");

            return BuildResponse_(200, document);
         }

      case RouteSpamTrapHitsClear:
         {
            const int removed = SpamTrapMonitor::ClearHits(0);

            if (removed < 0)
               return BuildResponse_(500, "{\"error\":\"the record could not be cleared; see the error log\"}");

            LOG_APPLICATION(Formatter::Format("RestApi: {0} cleared the record of spam trap hits ({1} removed).",
               String(caller.identity), removed));

            // hm_spamtraphits is traffic, so the statement chokepoint does not
            // record the delete. Destroying the record of what walked into the
            // traps is an administrative act all the same - and the one act
            // whose whole effect is to remove evidence - so it is recorded here,
            // under the actor the dispatch installed, as
            // Utilities.ClearSpamTrapHits records it over COM.
            AuditTrail::Instance()->RecordEvent(_T("spam trap hits"), _T("every recorded hit"), _T("cleared"),
               _T("the record of what has addressed a spam trap was deleted; the bans and the blocked senders it made stay"));

            AnsiString body;
            body.Format("{\"removed\":%d}", removed);

            return BuildResponse_(200, body);
         }

      case RouteSpamTrapDomainList:
         {
            std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(String(route.identifier));

            if (!domain)
               return BuildResponse_(404, "{\"error\":\"no such domain\"}");

            std::vector<SpamTrapMonitor::Listed> traps;

            if (!SpamTrapMonitor::List(domain, traps))
               return BuildResponse_(503, "{\"error\":\"the spam traps could not be read\"}");

            AnsiString json = "{\"domain\":\"";
            json += JsonEscape_(Utf8_(domain->GetName()));
            json += "\",\"spam_traps\":[";

            bool first = true;

            for (const SpamTrapMonitor::Listed &trap : traps)
            {
               if (!first)
                  json += ",";

               first = false;

               json += TrapJson_(trap.id, trap.address, trap.local_part, Time::GetTimeStampFromDateTime(trap.created));
            }

            json += "]}";

            return BuildResponse_(200, json);
         }

      case RouteSpamTrapDomainCreate:
         {
            std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(String(route.identifier));

            if (!domain)
               return BuildResponse_(404, "{\"error\":\"no such domain\"}");

            JsonValue body;

            if (!ParseObjectBody(requestBody, body))
               return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

            for (const std::pair<std::string, JsonValue> &member : body.Members())
            {
               if (member.first != "address")
                  return BuildResponse_(400, "{\"error\":\"unknown field: " + JsonEscape_(AnsiString(member.first.substr(0, 48).c_str())) + "\"}");
            }

            const JsonValue *address = body.Get("address");

            if (!address || !address->IsString() || address->AsString().empty())
               return BuildResponse_(400, "{\"error\":\"address is required: an address in this domain, or the part before the @\"}");

            String wanted;
            Unicode::MultiByteToWide(AnsiString(address->AsString().c_str()), wanted);

            String canonical;
            String refusal;

            const SpamTrapMonitor::AddResult result = SpamTrapMonitor::Add(domain, wanted, canonical, refusal);

            switch (result)
            {
            case SpamTrapMonitor::AddRefused:
               return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(Utf8_(refusal)) + "\"}");

            case SpamTrapMonitor::AddTaken:
               return BuildResponse_(409, "{\"error\":\"" + JsonEscape_(Utf8_(refusal)) + "\"}");

            case SpamTrapMonitor::AddFailed:
               return BuildResponse_(500, "{\"error\":\"" + JsonEscape_(Utf8_(refusal)) + "\"}");

            default:
               break;
            }

            SpamTrapMonitor::Listed entry;
            entry.address = canonical;

            const int at = LastAt(canonical);
            entry.local_part = at > 0 ? canonical.Left(at) : canonical;

            __int64 id = 0;
            if (PersistentSpamTrap::Find(domain->GetID(), entry.local_part, id))
               entry.id = id;

            std::vector<SpamTrapMonitor::Listed> traps;

            // For the created time, which the listing has and the add does not.
            if (SpamTrapMonitor::List(domain, traps))
            {
               for (const SpamTrapMonitor::Listed &listed : traps)
               {
                  if (listed.address == canonical)
                     entry = listed;
               }
            }

            return BuildResponse_(result == SpamTrapMonitor::AddCreated ? 201 : 200,
               TrapJson_(entry.id, entry.address, entry.local_part, Time::GetTimeStampFromDateTime(entry.created)));
         }

      case RouteSpamTrapDelete:
         {
            const String address = String(route.identifier);
            const int at = LastAt(address);

            if (at <= 0)
               return BuildResponse_(404, "{\"error\":\"no such spam trap\"}");

            std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(address.Mid(at + 1));

            if (!domain)
               return BuildResponse_(404, "{\"error\":\"no such spam trap\"}");

            String canonical;
            bool found = false;

            if (!SpamTrapMonitor::Remove(domain, address, canonical, found))
               return BuildResponse_(500, "{\"error\":\"the spam trap could not be removed; see the error log\"}");

            if (!found)
               return BuildResponse_(404, "{\"error\":\"no such spam trap\"}");

            return BuildResponse_(200, "{\"deleted\":true}");
         }

      default:
         break;
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }

   AnsiString
   RestApiServer::TrapJson_(__int64 id, const String &address, const String &localPart, const String &created)
   {
      AnsiString json;
      json.Format("{\"id\":%I64d,\"address\":\"%hs\",\"local_part\":\"%hs\",\"created\":\"%hs\"}",
         id,
         JsonEscape_(Utf8_(address)).c_str(),
         JsonEscape_(Utf8_(localPart)).c_str(),
         JsonEscape_(Utf8_(created)).c_str());

      return json;
   }
}
