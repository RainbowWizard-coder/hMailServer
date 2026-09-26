// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The inbound webhooks, over REST. See RestApiServer.h and Util/InboundWebhooks.h.
//
//    GET    /api/v1/inbound-webhooks                              the routes
//    POST   /api/v1/inbound-webhooks                              route an address or a domain to a URL
//    GET    /api/v1/inbound-webhooks/{address}                    one route
//    PUT    /api/v1/inbound-webhooks/{address}                    change it; a field left out keeps its value
//    DELETE /api/v1/inbound-webhooks/{address}                    remove it, its queue and what the queue kept
//    GET    /api/v1/inbound-webhooks/deliveries                   the queue, newest first
//    POST   /api/v1/inbound-webhooks/deliveries/{id}/retry        a kept delivery tried again
//    DELETE /api/v1/inbound-webhooks/deliveries/{id}              a delivery removed, with its message
//    GET    /api/v1/inbound-webhooks/deliveries/{id}/message      the message a delivery kept
//    POST   /api/v1/inbound-webhooks/run                          one delivery pass now
//
// WHO. view_configuration reads the routes and the queue; view_mailbox beside
// it reads a kept message, which is the mail itself; manage_inbound_webhooks
// changes - the permission an application's API key is given, since receiving
// mail is what it wants the server for. A route is judged in its own domain: a
// credential confined to named domains routes, reads and changes those domains'
// addresses and no other, the listings and the queue narrowed to them here, and
// a delivery of a route outside them is the same 404 as none. The pass is every
// domain's and is refused to such a credential in Authorize_. A route takes an
// address's mail instead of its mailbox, or copies it, so a route is made,
// changed or removed only by a credential that takes in every account it would
// reach (InboundWebhookReachesAccountBeyond_): the address's account, or every
// administrative account of a routed domain.
//
// WHERE A ROUTE MAY POINT. The server's administrator (the administrator
// password, or an account whose role holds manage_server and reaches every
// domain - never a key) may name this server's own network, as for an outbound
// webhook, but never a link-local or cloud metadata address. Every other
// credential names public addresses only, and a route it saves is held to that
// at every connection (InboundWebhooks.h): an application's key cannot make this
// server a way into the network it sits on.
//
// A SHARED SECRET IS NEVER RETURNED. As for an outbound webhook: it can be set
// and replaced, an empty string in a PUT keeps the one there is, and the answer
// says secret_set. The URL is answered whole only to a credential that may
// change the routes (manage_inbound_webhooks); any other reader - a domain's
// administrator, the auditor - is answered its scheme and host, since a
// receiver's URL often carries a token of its own, and the audit trail records
// it as changed and never by its value (AuditTrail.cpp).
//
// Retrying, removing and reading a kept delivery write no audited row - the
// queue is traffic - so each is recorded on the audit trail as an event: a kept
// message read, tried again or thrown away is somebody's mail.

#include "StdAfx.h"

#include "RestApiServer.h"

#include "AuditTrail.h"
#include "HttpsClient.h"
#include "InboundWebhooks.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "Parsing/StringParser.h"
#include "../Application/Application.h"
#include "../Application/RoleManager.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../Cache/CacheContainer.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/DatabaseConnectionManager.h"
#include "../SQL/SQLCommand.h"
#include "../../SMTP/PlusAddressing.h"

#include <ctime>
#include <set>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      String Utf8ToString(const std::string &utf8)
      {
         String value;
         Unicode::MultiByteToWide(AnsiString(utf8.c_str()), value);
         return value;
      }

      const char *StateWord(int state)
      {
         switch (state)
         {
         case InboundWebhooks::QueueDelivered: return "delivered";
         case InboundWebhooks::QueueKept:      return "kept";
         case InboundWebhooks::QueueBounced:   return "bounced";
         default:                              return "pending";
         }
      }

      int StateFromWord(const AnsiString &word)
      {
         if (word == "pending")   return InboundWebhooks::QueuePending;
         if (word == "delivered") return InboundWebhooks::QueueDelivered;
         if (word == "kept")      return InboundWebhooks::QueueKept;
         if (word == "bounced")   return InboundWebhooks::QueueBounced;

         return 0;
      }

      const char *FailureWord(int action)
      {
         return action == InboundWebhooks::FailureBounce ? "bounce" : "keep";
      }

      const char *FormatWord(int format)
      {
         return format == InboundWebhooks::FormatJson ? "json" : "form";
      }

      const char *AttachmentsWord(int policy)
      {
         switch (policy)
         {
         case InboundWebhooks::AttachmentsNames: return "names";
         case InboundWebhooks::AttachmentsNone:  return "none";
         default:                                return "include";
         }
      }

      // A route's URL as a reader who may not change the routes is answered it:
      // the scheme, the host and a port that is not the scheme's own, and
      // nothing of the path or the query, where a receiver's token lives.
      AnsiString UrlForReader(const AnsiString &url)
      {
         bool https = false;
         AnsiString host, port, path;

         if (!HttpsClient::ParseUrl(url, https, host, port, path))
            return "(withheld)";

         AnsiString shown = https ? "https://" : "http://";

         if (host.Find(":") >= 0)
            shown += "[" + host + "]";
         else
            shown += host;

         if (port != (https ? "443" : "80"))
            shown += ":" + port;

         shown += "/...";
         return shown;
      }

      bool ParseOptionalObject(const AnsiString &requestBody, JsonValue &body)
      {
         AnsiString trimmed = requestBody;
         trimmed.Trim();

         std::string text = trimmed.IsEmpty() ? std::string("{}") : std::string(trimmed.c_str(), (size_t) trimmed.GetLength());

         std::string parseError;
         return JsonValue::Parse(text, body, parseError) && body.IsObject();
      }

      const char *KnownKeys[] =
      {
         "address", "url", "secret", "deliver", "on_failure", "format", "attachments",
         "include_raw", "max_size_kb", "enabled"
      };

      bool OnlyKnownKeys(const JsonValue &body, AnsiString &unknown)
      {
         for (const std::pair<std::string, JsonValue> &member : body.Members())
         {
            bool known = false;

            for (const char *candidate : KnownKeys)
            {
               if (member.first == candidate)
               {
                  known = true;
                  break;
               }
            }

            if (!known)
            {
               unknown = AnsiString(member.first.substr(0, 48).c_str());
               return false;
            }
         }

         return true;
      }

      // A word, read into one of the route's small numbers. False, with the
      // sentence, for anything else.
      bool ReadWord(const JsonValue &body, const char *key, const char *const *words, int count, int &value, AnsiString &error)
      {
         const JsonValue *member = body.Get(key);
         if (member == nullptr || member->IsNull())
            return true;

         if (member->IsString())
         {
            for (int i = 0; i < count; i++)
            {
               if (member->AsString() == words[i])
               {
                  value = i;
                  return true;
               }
            }
         }

         error = AnsiString(key) + " is one of:";
         for (int i = 0; i < count; i++)
         {
            error += i == 0 ? " " : ", ";
            error += words[i];
         }

         return false;
      }

      bool ReadBool(const JsonValue &body, const char *key, bool &value, AnsiString &error)
      {
         const JsonValue *member = body.Get(key);
         if (member == nullptr || member->IsNull())
            return true;

         if (!member->IsBool())
         {
            error = AnsiString(key) + " is true or false";
            return false;
         }

         value = member->AsBool();
         return true;
      }
   }

   bool
   RestApiServer::InboundWebhookReachesAccountBeyond_(const Caller &caller, const String &routeAddress)
   {
      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(InboundWebhooks::DomainOf(routeAddress));

      if (routeAddress.Find(_T("@")) >= 0)
      {
         // The address as written, and the account its plus tag names: a
         // route on either takes that account's mail (InboundWebhooks::FindRoute).
         std::vector<String> names;
         names.push_back(routeAddress);
         if (domain)
            names.push_back(PlusAddressing::ExtractAccountAddress(routeAddress, domain));

         for (const String &name : names)
         {
            std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(name);
            if (account && !CallerTakesInAccount_(caller, account))
               return true;
         }

         return false;
      }

      // A domain's route takes the mail of every address of the domain without
      // a route of its own - its accounts' included. An account with no admin
      // level and no role holds no authority and is taken in by any credential,
      // so the ones to ask are those with an admin level and those with a role.
      if (!domain)
         return false;

      std::set<__int64> candidates;

      SQLCommand command("select accountid from hm_accounts where accountdomainid = @DOMAINID and accountadminlevel > 0");
      command.AddParameter("@DOMAINID", (__int64) domain->GetID());

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return true;   // what the domain holds cannot be read: refused, the safer answer

      while (!recordset->IsEOF())
      {
         candidates.insert(recordset->GetInt64Value("accountid"));
         recordset->MoveNext();
      }

      for (const RoleAssignment &assignment : RoleManager::ListAssignments())
         candidates.insert(assignment.account_id);

      for (__int64 id : candidates)
      {
         std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(id);
         if (!account || account->GetDomainID() != domain->GetID())
            continue;

         if (!CallerTakesInAccount_(caller, account))
            return true;
      }

      return false;
   }

   HttpResponse
   RestApiServer::HandleInboundWebhooks_(const Caller &caller, const Route &route, const AnsiString &requestBody)
   {
      AnsiString (*escape)(const AnsiString &) = &RestApiServer::JsonEscape_;

      // Whether this credential reaches a route's domain. The path's domain
      // was judged in Authorize_; this is for a route named in a body, and for
      // the listings and the queue, which no path narrows.
      auto reaches = [&](const String &routeAddress) -> bool
      {
         return IsDomainAllowed_(caller.domains, InboundWebhooks::DomainOf(routeAddress));
      };

      // The refusal of a route that would reach an account this credential
      // does not take in (InboundWebhookReachesAccountBeyond_): the take-in
      // rule's sentence, as every change to what is in an account answers.
      auto refuseBeyond = [&](const String &routeAddress) -> HttpResponse
      {
         LOG_APPLICATION("RestApi: Refused a change to the inbound webhook for " + routeAddress + " by " + String(caller.identity) +
            ": it would reach an account that holds authority the credential does not.");
         return AccountNotTakenIn_();
      };

      // Whether this credential may change the routes, and so is answered their
      // URLs whole.
      const bool managesRoutes = CallerManagesInboundWebhooks_(caller);

      // A route as an answer. Note what is NOT here: the secret. The one thing
      // said about it is whether there is one. And the URL is the whole of it
      // only for a credential that may change the route.
      auto routeJson = [&](const InboundWebhooks::Route &hook) -> AnsiString
      {
         int pending = 0;
         int delivered = 0;
         int kept = 0;
         int bounced = 0;
         InboundWebhooks::Instance()->CountDeliveries(hook.id, pending, delivered, kept, bounced);

         const AnsiString url = Utf8_(hook.url);

         AnsiString json;
         json += "{";
         json += "\"address\":\"" + escape(Utf8_(hook.address)) + "\",";
         json += "\"url\":\"" + escape(managesRoutes ? url : UrlForReader(url)) + "\",";
         json += "\"secret_set\":" + AnsiString(hook.secret.IsEmpty() ? "false" : "true") + ",";
         json += "\"deliver\":" + AnsiString(hook.deliver ? "true" : "false") + ",";
         json += "\"on_failure\":\"" + AnsiString(FailureWord(hook.on_failure)) + "\",";
         json += "\"format\":\"" + AnsiString(FormatWord(hook.format)) + "\",";
         json += "\"attachments\":\"" + AnsiString(AttachmentsWord(hook.attachments)) + "\",";
         json += "\"include_raw\":" + AnsiString(hook.include_raw ? "true" : "false") + ",";
         json += "\"private_network\":" + AnsiString(hook.private_network ? "true" : "false") + ",";
         json += "\"enabled\":" + AnsiString(hook.enabled ? "true" : "false") + ",";

         AnsiString numbers;
         numbers.Format("\"max_size_kb\":%d,\"created\":%I64d,\"pending\":%d,\"delivered\":%d,\"kept\":%d,\"bounced\":%d",
            hook.max_size_kb, hook.created, pending, delivered, kept, bounced);
         json += numbers;

         json += "}";

         return json;
      };

      auto deliveryJson = [&](const InboundWebhooks::Delivery &delivery) -> AnsiString
      {
         AnsiString numbers;
         numbers.Format("{\"id\":%I64d,\"created\":%I64d,\"attempts\":%d,\"next_attempt\":%I64d,\"queue_id\":%I64d,\"size\":%I64d,",
            delivery.id, delivery.created, delivery.tries, delivery.next, delivery.queue_id, delivery.size);

         AnsiString json = numbers;
         json += "\"route\":\"" + escape(Utf8_(delivery.route)) + "\",";
         json += "\"state\":\"" + AnsiString(StateWord(delivery.state)) + "\",";
         json += "\"sender\":\"" + escape(Utf8_(delivery.sender)) + "\",";
         json += "\"recipients\":\"" + escape(Utf8_(delivery.recipients)) + "\",";
         json += "\"spam\":" + AnsiString(delivery.spam ? "true" : "false") + ",";
         json += "\"message_kept\":" + AnsiString(delivery.file.IsEmpty() ? "false" : "true") + ",";
         json += "\"error\":\"" + escape(Utf8_(delivery.error)) + "\"}";

         return json;
      };

      static const char *const FailureWords[] = { "keep", "bounce" };
      static const char *const FormatWords[] = { "form", "json" };
      static const char *const AttachmentWords[] = { "include", "names", "none" };

      switch (route.kind)
      {
      case RouteInboundWebhookList:
         {
            std::vector<InboundWebhooks::Route> hooks;

            if (!InboundWebhooks::Instance()->GetRoutes(hooks))
               return BuildResponse_(500, "{\"error\":\"the inbound webhooks could not be read\"}");

            AnsiString body = "{\"inbound_webhooks\":[";
            bool first = true;

            for (const InboundWebhooks::Route &hook : hooks)
            {
               if (!reaches(hook.address))
                  continue;

               if (!first)
                  body += ",";

               body += routeJson(hook);
               first = false;
            }

            body += "]}";

            return BuildResponse_(200, body);
         }

      case RouteInboundWebhookGet:
         {
            InboundWebhooks::Route hook;

            if (!InboundWebhooks::Instance()->GetRoute(String(route.identifier), hook))
               return BuildResponse_(404, "{\"error\":\"no inbound webhook routes that address\"}");

            return BuildResponse_(200, routeJson(hook));
         }

      case RouteInboundWebhookCreate:
      case RouteInboundWebhookUpdate:
         {
            const bool creating = route.kind == RouteInboundWebhookCreate;

            JsonValue body;
            if (!ParseOptionalObject(requestBody, body))
               return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

            AnsiString unknown;
            if (!OnlyKnownKeys(body, unknown))
               return BuildResponse_(400, "{\"error\":\"unknown field: " + escape(unknown) + "\"}");

            InboundWebhooks::Route hook;

            if (creating)
            {
               // A route made where nothing says otherwise: it posts only, keeps
               // what it could not post, sends SendGrid's form with the
               // attachments, and takes messages of up to 10 MB.
               hook.max_size_kb = 10240;

               const JsonValue *address = body.Get("address");
               if (address == nullptr || !address->IsString())
                  return BuildResponse_(400, "{\"error\":\"name the address or the domain to route\"}");

               hook.address = InboundWebhooks::NormaliseAddress(Utf8ToString(address->AsString()));
               if (hook.address.IsEmpty())
                  return BuildResponse_(400, "{\"error\":\"an inbound webhook routes an address (user@example.com) or a whole domain (example.com)\"}");

               // The domain in the body, judged as Authorize_ judges one in a
               // path: a credential confined to named domains routes those.
               if (!reaches(hook.address))
                  return BuildResponse_(403, "{\"error\":\"this credential is not permitted for that domain\"}");

               if (InboundWebhookReachesAccountBeyond_(caller, hook.address))
                  return refuseBeyond(hook.address);
            }
            else
            {
               // A field the body leaves out keeps its value, so switching a
               // route off does not need its secret sent back.
               if (!InboundWebhooks::Instance()->GetRoute(String(route.identifier), hook))
                  return BuildResponse_(404, "{\"error\":\"no inbound webhook routes that address\"}");

               if (InboundWebhookReachesAccountBeyond_(caller, hook.address))
                  return refuseBeyond(hook.address);

               const JsonValue *address = body.Get("address");
               if (address != nullptr &&
                   (!address->IsString() || InboundWebhooks::NormaliseAddress(Utf8ToString(address->AsString())) != hook.address))
                  return BuildResponse_(400, "{\"error\":\"a route's address is the one in the path; remove the route and make another to route another address\"}");
            }

            const JsonValue *member = nullptr;

            if ((member = body.Get("url")) != nullptr)
            {
               if (!member->IsString())
                  return BuildResponse_(400, "{\"error\":\"url is a string\"}");

               hook.url = Utf8ToString(member->AsString());
            }

            // Write-only. An empty string keeps what is there rather than
            // clearing it: a route with no secret is refused anyway, and a client
            // that sent secret:"" by accident would otherwise break a working
            // endpoint.
            if ((member = body.Get("secret")) != nullptr && member->IsString())
            {
               const String given = Utf8ToString(member->AsString());

               if (!given.IsEmpty())
                  hook.secret = given;
            }

            AnsiString error;

            if (!ReadBool(body, "deliver", hook.deliver, error) ||
                !ReadBool(body, "include_raw", hook.include_raw, error) ||
                !ReadBool(body, "enabled", hook.enabled, error) ||
                !ReadWord(body, "on_failure", FailureWords, 2, hook.on_failure, error) ||
                !ReadWord(body, "format", FormatWords, 2, hook.format, error) ||
                !ReadWord(body, "attachments", AttachmentWords, 3, hook.attachments, error))
               return BuildResponse_(400, "{\"error\":\"" + escape(error) + "\"}");

            if ((member = body.Get("max_size_kb")) != nullptr)
            {
               if (!member->IsNumber())
                  return BuildResponse_(400, "{\"error\":\"max_size_kb is a whole number\"}");

               // At most the 80 MB MessageData parses (InboundWebhooks::MaxSizeKb):
               // a larger message could not be made into a body at all.
               const __int64 ceiling = member->AsInt64();
               if (ceiling < 0 || ceiling > (__int64) InboundWebhooks::MaxSizeKb)
                  return BuildResponse_(400, "{\"error\":\"a size ceiling is between 0 (none of the route's own: 81920 KB, the most any route posts) and 81920 KB\"}");

               hook.max_size_kb = (int) ceiling;
            }

            String refusal;
            if (!InboundWebhooks::Instance()->SaveRoute(hook, creating, CallerMayRouteToPrivateNetwork_(caller), refusal))
            {
               AnsiString answer;
               answer.Format("{\"error\":\"%hs\"}", escape(Utf8_(refusal)).c_str());

               // An address that has a route is a conflict, not a bad request.
               return BuildResponse_(creating && refusal.Find(_T("exists already")) >= 0 ? 409 : 400, answer);
            }

            LOG_APPLICATION("RestApi: Inbound webhook for " + hook.address + " " +
               String(creating ? _T("created") : _T("changed")) + " by " + String(caller.identity) + ".");

            InboundWebhooks::Route stored;
            if (!InboundWebhooks::Instance()->GetRoute(hook.address, stored))
               return BuildResponse_(500, "{\"error\":\"the inbound webhook was saved but could not be read back\"}");

            return BuildResponse_(creating ? 201 : 200, routeJson(stored));
         }

      case RouteInboundWebhookDelete:
         {
            InboundWebhooks::Route hook;
            if (!InboundWebhooks::Instance()->GetRoute(String(route.identifier), hook))
               return BuildResponse_(404, "{\"error\":\"no inbound webhook routes that address\"}");

            if (InboundWebhookReachesAccountBeyond_(caller, hook.address))
               return refuseBeyond(hook.address);

            if (!InboundWebhooks::Instance()->DeleteRoute(hook.address))
               return BuildResponse_(500, "{\"error\":\"the inbound webhook could not be removed\"}");

            LOG_APPLICATION("RestApi: Inbound webhook for " + hook.address + " deleted by " + String(caller.identity) + ".");

            return BuildResponse_(200, "{\"deleted\":true}");
         }

      case RouteInboundWebhookDeliveryList:
         {
            const AnsiString routeName = QueryParameter_(route.query, "route");
            const AnsiString stateWord = QueryParameter_(route.query, "state");

            if (!stateWord.IsEmpty() && StateFromWord(stateWord) == 0)
               return BuildResponse_(400, "{\"error\":\"state is pending, delivered, kept or bounced\"}");

            int limit = 100;

            const AnsiString limitText = QueryParameter_(route.query, "limit");
            if (!limitText.IsEmpty())
               limit = atoi(limitText.c_str());

            if (limit < 1)
               limit = 1;
            if (limit > 1000)
               limit = 1000;

            String routeAddress;
            Unicode::MultiByteToWide(routeName, routeAddress);

            if (!routeAddress.IsEmpty() && !reaches(routeAddress))
               return BuildResponse_(200, "{\"deliveries\":[]}");

            // A credential confined to named domains is read its own routes'
            // rows and no other: the queue passes the rest over while it reads,
            // so that a busier domain's newest rows cannot fill the page and
            // leave this one's off it.
            std::set<__int64> ownRoutes;
            const bool confined = !caller.domains.empty();

            if (confined)
            {
               std::vector<InboundWebhooks::Route> hooks;
               if (!InboundWebhooks::Instance()->GetRoutes(hooks))
                  return BuildResponse_(500, "{\"error\":\"the inbound webhooks could not be read\"}");

               for (const InboundWebhooks::Route &hook : hooks)
               {
                  if (reaches(hook.address))
                     ownRoutes.insert(hook.id);
               }
            }

            std::vector<InboundWebhooks::Delivery> deliveries;

            if (!InboundWebhooks::Instance()->GetDeliveries(routeAddress, StateFromWord(stateWord), limit, deliveries,
                                                             confined ? &ownRoutes : nullptr))
               return BuildResponse_(500, "{\"error\":\"the inbound webhook queue could not be read\"}");

            AnsiString body = "{\"deliveries\":[";
            int written = 0;

            for (const InboundWebhooks::Delivery &delivery : deliveries)
            {
               if (written >= limit)
                  break;

               if (!reaches(delivery.route))
                  continue;

               if (written > 0)
                  body += ",";

               body += deliveryJson(delivery);
               written++;
            }

            body += "]}";

            return BuildResponse_(200, body);
         }

      case RouteInboundWebhookDeliveryRetry:
      case RouteInboundWebhookDeliveryDelete:
      case RouteInboundWebhookDeliveryMessage:
         {
            InboundWebhooks::Delivery delivery;

            // A delivery of a route outside the credential's domains is the
            // same 404 as none, so that a number tried is not an oracle.
            if (!InboundWebhooks::Instance()->GetDelivery(route.record_id, delivery) || !reaches(delivery.route))
               return BuildResponse_(404, "{\"error\":\"no such delivery\"}");

            const String label = delivery.route + _T(" #") + StringParser::IntToString(delivery.id);
            const String detail = Formatter::Format("message {0} from {1} to {2}", delivery.queue_id, delivery.sender, delivery.recipients);

            if (route.kind == RouteInboundWebhookDeliveryRetry)
            {
               String refusal;
               if (!InboundWebhooks::Instance()->RetryDelivery(delivery.id, refusal))
               {
                  AnsiString answer;
                  answer.Format("{\"error\":\"%hs\"}", escape(Utf8_(refusal)).c_str());
                  return BuildResponse_(409, answer);
               }

               AuditTrail::Instance()->RecordEvent(_T("inbound webhook delivery"), label, _T("retried"), detail);

               LOG_APPLICATION("RestApi: Inbound webhook delivery " + StringParser::IntToString(delivery.id) +
                               " queued again by " + String(caller.identity) + ".");

               return BuildResponse_(200, "{\"retried\":true}");
            }

            if (route.kind == RouteInboundWebhookDeliveryDelete)
            {
               if (!InboundWebhooks::Instance()->DeleteDelivery(delivery.id))
                  return BuildResponse_(500, "{\"error\":\"the delivery could not be removed\"}");

               AuditTrail::Instance()->RecordEvent(_T("inbound webhook delivery"), label, _T("removed"), detail);

               LOG_APPLICATION("RestApi: Inbound webhook delivery " + StringParser::IntToString(delivery.id) +
                               " removed by " + String(caller.identity) + ".");

               return BuildResponse_(200, "{\"deleted\":true}");
            }

            // The message a delivery kept, as it arrived.
            AnsiString bytes;
            if (!InboundWebhooks::Instance()->ReadDeliveryMessage(delivery.id, bytes))
               return BuildResponse_(404, "{\"error\":\"this delivery keeps no message: it was posted, returned to its sender or removed\"}");

            AuditTrail::Instance()->RecordEvent(_T("inbound webhook delivery"), label, _T("downloaded"), detail);

            LOG_APPLICATION("RestApi: The message kept by inbound webhook delivery " + StringParser::IntToString(delivery.id) +
                            " downloaded by " + String(caller.identity) + ".");

            AnsiString fileName;
            fileName.Format("inbound-webhook-delivery-%I64d.eml", delivery.id);

            HttpResponse response;
            response.status = 200;
            response.content_type = "message/rfc822";
            response.body = bytes;
            response.no_compress = true;
            response.extra_headers =
               "Content-Disposition: attachment; filename=\"" + fileName + "\"\r\n"
               "X-Content-Type-Options: nosniff\r\n"
               "Content-Security-Policy: sandbox\r\n"
               "Cache-Control: no-store\r\n";
            return response;
         }

      case RouteInboundWebhookRun:
         {
            // One pass now, rather than in the minute. One runs at a time and
            // nobody waits for one: a pass asked for while another is running
            // is not made - running says so - and the running pass posts
            // what was queued meanwhile when it ends.
            const int delivered = InboundWebhooks::Instance()->RunDeliveries();
            const int removed = InboundWebhooks::Instance()->PurgeOldDeliveries();

            AnsiString answer;
            answer.Format("{\"delivered\":%d,\"removed\":%d,\"running\":%s}", delivered < 0 ? 0 : delivered, removed,
               delivered < 0 ? "true" : "false");

            return BuildResponse_(200, answer);
         }

      default:
         break;
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }

   AnsiString
   RestApiServer::OpenApiInboundWebhooksPaths_()
   {
      static const char *paths =
         ",\"/api/v1/inbound-webhooks\":{\"get\":{\"summary\":\"The inbound webhooks\","
         "\"description\":\"Each address or domain whose mail this server posts to a URL: its address, the URL (whole for a credential holding manage_inbound_webhooks; its scheme and host alone for any other reader, since a receiver's URL often carries a token of its own), whether a shared secret is set (the secret itself is never returned), whether the mailbox the address has receives the message as well (deliver), what a message the endpoint never accepted becomes (on_failure: keep or bounce), the body's format (form, SendGrid's multipart/form-data fields, or json), what is done with attachments (include, names or none), whether the message as it arrived is sent as well (include_raw), the route's own size ceiling in KB (max_size_kb; 0 for none of its own, which is 81920, the most any route posts), whether a route may reach this server's own network (private_network: made by the server's administrator), whether it is on, and how many of its deliveries are pending, delivered, kept and bounced. Needs view_configuration; a credential confined to named domains is listed the routes of those domains. docs/InboundWebhooks.md.\","
         "\"responses\":{\"200\":{\"description\":\"inbound_webhooks\"}}},"
         "\"post\":{\"summary\":\"Route an address or a domain to a URL\","
         "\"description\":\"Body: address (required; an address, user@example.com, or a whole domain, example.com, of a domain this server hosts - an address route wins over its domain's, and a recipient with a plus tag is found by the address before the tag), url (required; https, or plain http only to this machine, since the body is the message itself - a URL the server's administrator names may be on this server's own network but never a link-local or cloud metadata address, and one any other credential names, an API key's among them, must lead to public addresses only), secret (required; the shared secret each request is signed under as an outbound webhook's is, protected at rest and never returned), deliver (default false: the route alone receives the address's mail; true delivers it to the address's mailbox as well, where it has one), on_failure (keep or bounce; default keep - a message the endpoint did not accept in WebhookMaxAttempts attempts is kept for the administrator, or returned to its sender), format (form or json; default form), attachments (include, names or none; default include), include_raw (default false), max_size_kb (default 10240: a message larger than this is not posted but kept or returned as on_failure says; at most 81920, the 80 MB the message parser takes, and 0 for no ceiling of the route's own, which is then 81920) and enabled (default true). An address with a route is taken whether or not a mailbox has it. Needs manage_inbound_webhooks, in the route's domain.\","
         "\"requestBody\":{\"required\":true,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"address\",\"url\",\"secret\"],\"properties\":{"
         "\"address\":{\"type\":\"string\",\"description\":\"user@example.com, or example.com for every address of the domain.\"},"
         "\"url\":{\"type\":\"string\",\"description\":\"https://host/path, with no space or control character. The server's administrator may name this server's own network; any other credential, a public address. Read back whole only by a credential holding manage_inbound_webhooks, and recorded on the audit trail as changed, never by its value.\"},"
         "\"secret\":{\"type\":\"string\",\"writeOnly\":true,\"description\":\"Write-only. Each request is signed under it; an empty string in a change keeps the one that is set.\"},"
         "\"deliver\":{\"type\":\"boolean\",\"description\":\"Deliver to the address's mailbox as well, where it has one.\"},"
         "\"on_failure\":{\"type\":\"string\",\"enum\":[\"keep\",\"bounce\"],\"description\":\"What a message the endpoint never accepted becomes.\"},"
         "\"format\":{\"type\":\"string\",\"enum\":[\"form\",\"json\"],\"description\":\"form: multipart/form-data in SendGrid's Inbound Parse field names. json: one JSON document.\"},"
         "\"attachments\":{\"type\":\"string\",\"enum\":[\"include\",\"names\",\"none\"],\"description\":\"include: carried as files. names: listed with their names, types and sizes only. none: left out.\"},"
         "\"include_raw\":{\"type\":\"boolean\",\"description\":\"Send the message as it arrived as well: the email field of the form, or raw (base64) in JSON.\"},"
         "\"max_size_kb\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":81920,\"description\":\"A larger message is not posted. At most 81920, the 80 MB the message parser takes; 0: no ceiling of the route's own, which is then 81920.\"},"
         "\"enabled\":{\"type\":\"boolean\"}"
         "}}}}},"
         "\"responses\":{\"201\":{\"description\":\"The route as it now stands\"},\"400\":{\"description\":\"A field was refused, with the reason\"},\"403\":{\"description\":\"The credential does not reach the address's domain, or the route would reach an account that holds authority this credential does not: the address's, or one of the domain's with an admin level or a role\"},\"409\":{\"description\":\"The address has a route already\"}}}}"
         ",\"/api/v1/inbound-webhooks/{address}\":{\"get\":{\"summary\":\"One inbound webhook\","
         "\"description\":\"As the listing shows it. The shared secret is never returned. Needs view_configuration in the address's domain.\","
         "\"parameters\":[{\"name\":\"address\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"The route\"},\"404\":{\"description\":\"No route for that address\"}}},"
         "\"put\":{\"summary\":\"Change an inbound webhook\","
         "\"description\":\"Any subset of url, secret, deliver, on_failure, format, attachments, include_raw, max_size_kb and enabled; a field left out keeps its value, and an empty secret keeps the one that is set. The address is the path's. The URL is judged again for the credential making the change: a route a credential other than the server's administrator saves reaches public addresses only from then on. Needs manage_inbound_webhooks in the address's domain.\","
         "\"parameters\":[{\"name\":\"address\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"requestBody\":{\"required\":false,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
         "\"url\":{\"type\":\"string\"},\"secret\":{\"type\":\"string\",\"writeOnly\":true},\"deliver\":{\"type\":\"boolean\"},"
         "\"on_failure\":{\"type\":\"string\",\"enum\":[\"keep\",\"bounce\"]},\"format\":{\"type\":\"string\",\"enum\":[\"form\",\"json\"]},"
         "\"attachments\":{\"type\":\"string\",\"enum\":[\"include\",\"names\",\"none\"]},\"include_raw\":{\"type\":\"boolean\"},"
         "\"max_size_kb\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":81920},\"enabled\":{\"type\":\"boolean\"}}}}}},"
         "\"responses\":{\"200\":{\"description\":\"The route as saved\"},\"400\":{\"description\":\"A field was refused, with the reason\"},\"403\":{\"description\":\"The route would reach an account that holds authority this credential does not: the address's, or one of the domain's with an admin level or a role\"},\"404\":{\"description\":\"No route for that address\"}}},"
         "\"delete\":{\"summary\":\"Remove an inbound webhook\","
         "\"description\":\"Removes the route, its queue and every message the queue kept. The address is then what it would be with no route. Needs manage_inbound_webhooks in the address's domain.\","
         "\"parameters\":[{\"name\":\"address\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"Removed\"},\"403\":{\"description\":\"The route would reach an account that holds authority this credential does not: the address's, or one of the domain's with an admin level or a role\"},\"404\":{\"description\":\"No route for that address\"}}}}"
         ",\"/api/v1/inbound-webhooks/deliveries\":{\"get\":{\"summary\":\"The inbound webhook queue\","
         "\"description\":\"Newest first: the id (the X-hMailServer-Delivery header's value), the route, when the message was taken, its state (pending, delivered, kept - never accepted, and kept for the administrator - or bounced, returned to its sender), the attempts made, when the next is due, the delivery queue's id for the message, its sender and the route's share of its recipients, its size, whether it was classified as spam, whether a message is still kept for it, and the last failure. Query: route (an address), state and limit (1 to 1000, default 100). Needs view_configuration; a credential confined to named domains is listed its own domains' deliveries.\","
         "\"parameters\":["
         "{\"name\":\"route\",\"in\":\"query\",\"schema\":{\"type\":\"string\"}},"
         "{\"name\":\"state\",\"in\":\"query\",\"schema\":{\"type\":\"string\",\"enum\":[\"pending\",\"delivered\",\"kept\",\"bounced\"]}},"
         "{\"name\":\"limit\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"}}"
         "],\"responses\":{\"200\":{\"description\":\"deliveries\"},\"400\":{\"description\":\"state is not one of the four words\"}}}}"
         ",\"/api/v1/inbound-webhooks/deliveries/{id}\":{\"delete\":{\"summary\":\"Remove an inbound webhook delivery\","
         "\"description\":\"The delivery and the message it kept, which is gone for good. On the audit trail. Needs manage_inbound_webhooks in the route's domain.\","
         "\"parameters\":[{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"integer\"}}],"
         "\"responses\":{\"200\":{\"description\":\"Removed\"},\"404\":{\"description\":\"No such delivery\"}}}}"
         ",\"/api/v1/inbound-webhooks/deliveries/{id}/retry\":{\"post\":{\"summary\":\"Try an inbound webhook delivery again\","
         "\"description\":\"A kept delivery is made due now with its attempts at none, and a pending one is made due now; the attempt is made at once. Refused (409) for one delivered or returned to its sender, and for one whose message is gone. On the audit trail. Needs manage_inbound_webhooks in the route's domain.\","
         "\"parameters\":[{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"integer\"}}],"
         "\"responses\":{\"200\":{\"description\":\"Queued again\"},\"404\":{\"description\":\"No such delivery\"},\"409\":{\"description\":\"Nothing to try again, with the reason\"}}}}"
         ",\"/api/v1/inbound-webhooks/deliveries/{id}/message\":{\"get\":{\"summary\":\"The message an inbound webhook delivery kept\","
         "\"description\":\"The message as it arrived, as message/rfc822 with Content-Disposition, nosniff, sandbox and no-store. On the audit trail as downloaded. Needs view_configuration and view_mailbox, in the route's domain: it hands over the mail itself.\","
         "\"parameters\":[{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"integer\"}}],"
         "\"responses\":{\"200\":{\"description\":\"The message\"},\"404\":{\"description\":\"No such delivery, or it keeps no message\"}}}}"
         ",\"/api/v1/inbound-webhooks/run\":{\"post\":{\"summary\":\"Run one inbound webhook pass now\","
         "\"description\":\"Attempts every delivery that is due and removes the delivered and returned ones WebhookRetentionDays no longer keeps - the pass the scheduled task makes every minute, and a drain makes as soon as a message is queued. One pass runs at a time and nobody waits for one: a pass asked for while another runs is not made, running says so, and the running pass posts what was queued meanwhile when it ends. delivered counts what this pass sent, which may be nothing. Needs manage_inbound_webhooks, and a credential that reaches every domain.\","
         "\"responses\":{\"200\":{\"description\":\"delivered, removed and running\"}}}}";

      return AnsiString(paths);
   }
}
