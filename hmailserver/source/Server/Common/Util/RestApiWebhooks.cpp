// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The outbound webhooks, over REST. See RestApiServer.h and Util/WebhookManager.h.
//
//    GET    /api/v1/webhooks              the subscriptions
//    POST   /api/v1/webhooks              create one
//    GET    /api/v1/webhooks/{name}       one of them
//    PUT    /api/v1/webhooks/{name}       change one; a field left out keeps its value
//    DELETE /api/v1/webhooks/{name}       remove it, and everything of its still queued
//    GET    /api/v1/webhooks/deliveries   the queue, newest first
//    POST   /api/v1/webhooks/run          one delivery pass now
//    POST   /api/v1/webhooks/test         raise an event on request
//
// The deliveries and the two verbs sit under the collection and not under a
// name, which is why a subscription cannot be called "deliveries", "run" or
// "test": the router matches those three first.
//
// A SHARED SECRET IS NEVER RETURNED. It can be set, it can be replaced, and an
// empty string in a PUT leaves the one that is there - a client that wants to
// clear it deletes the subscription, because a subscription with no secret
// would send bodies nobody can verify and SaveSubscription refuses one. The
// answer says secret_set, which is what an administrator needs to know and what
// a reader of the page must not be able to take away. That, and the fact that a
// subscription is the address this server will POST to, is why no API key
// reaches any of these routes whatever its scope: see IsApiKeyRoute_ in
// RestApiServer.cpp. An account that holds a role does - view_configuration to
// read, manage_settings to delete one, manage_server to create or change one,
// raise a test or run a pass - and a role confined to named domains is refused
// all eight as server-wide, since a subscription reaches every domain it is not
// confined to.
//
// WHERE AN ENDPOINT MAY BE. Anywhere this server can reach over https, and
// that includes addresses only this server can reach - a receiver on the same
// network is the common case, so the private ranges are not refused the way
// HttpsClient::RequestPublic refuses them for a URL an outsider chose. The
// consequence is stated rather than hidden: whoever can create a subscription
// can make this server connect to a host and port of their choosing and read
// whether it answered, refused or timed out. That is why creating one is
// manage_server's and no API key's, and why plain http is refused off this
// machine. The secret is protected at rest the way a route's password is.
//
// POST /test is the "does this work" button. It raises a real event through
// exactly the path a real one takes - the same matching, the same domain
// switches, the same ceiling - so what arrives is what would arrive, and the
// answer says how many subscriptions took it rather than pretending one did.

#include "StdAfx.h"

#include "RestApiServer.h"

#include "JsonDocument.h"
#include "Unicode.h"
#include "WebhookManager.h"

#include <ctime>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A JSON string is UTF-8 bytes; a String is characters. CStdStr's own
      // conversion between the two is CP_ACP, the machine's code page, which
      // loses whatever that page does not have - so the same conversion the
      // rest of this API uses (RestApiAdministration::Utf8ToString) is used here.
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
         case WebhookManager::QueueDelivered:    return "delivered";
         case WebhookManager::QueueDeadLettered: return "dead-lettered";
         default:                                return "pending";
         }
      }

      int StateFromWord(const AnsiString &word)
      {
         if (word == "pending")       return WebhookManager::QueuePending;
         if (word == "delivered")     return WebhookManager::QueueDelivered;
         if (word == "dead-lettered") return WebhookManager::QueueDeadLettered;

         return 0;
      }

      // An absent or empty body is an empty object: every member of every body
      // here except the test event's is optional.
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
         "name", "url", "secret", "events", "domains", "addresses",
         "enabled", "max_per_minute", "include_subject"
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

      // A member that may be given either as a JSON array of strings or as the
      // comma-separated string the row holds. The array is what a program writes
      // and what the Deck sends; the string is what a person types into hmctl.
      bool ReadList(const JsonValue &body, const char *key, String &value, AnsiString &error)
      {
         const JsonValue *member = body.Get(key);
         if (member == nullptr || member->IsNull())
            return true;

         if (member->IsString())
         {
            value = Utf8ToString(member->AsString());
            return true;
         }

         if (!member->IsArray())
         {
            error = AnsiString(key) + " is a list of strings, or the same list as one comma-separated string";
            return false;
         }

         String joined;

         for (const JsonValue &item : member->Items())
         {
            if (!item.IsString())
            {
               error = AnsiString(key) + " holds strings";
               return false;
            }

            String one = Utf8ToString(item.AsString());
            one.Trim();

            if (one.IsEmpty())
               continue;

            if (!joined.IsEmpty())
               joined += _T(",");

            joined += one;
         }

         value = joined;
         return true;
      }
   }

   HttpResponse
   RestApiServer::HandleWebhooks_(const Caller &caller, const Route &route, const AnsiString &requestBody)
   {
      AnsiString (*escape)(const AnsiString &) = &RestApiServer::JsonEscape_;

      // The subscription as an answer. Note what is NOT here: whsecret. The one
      // thing said about it is whether there is one.
      auto subscriptionJson = [&](const WebhookManager::Subscription &subscription) -> AnsiString
      {
         int pending = 0;
         int delivered = 0;
         int deadLettered = 0;
         WebhookManager::Instance()->CountDeliveries(subscription.id, pending, delivered, deadLettered);

         AnsiString json;
         json += "{";
         json += "\"name\":\"" + escape(Utf8_(subscription.name)) + "\",";
         json += "\"url\":\"" + escape(Utf8_(subscription.url)) + "\",";
         json += "\"secret_set\":" + AnsiString(subscription.secret.IsEmpty() ? "false" : "true") + ",";
         json += "\"events\":\"" + escape(Utf8_(subscription.events)) + "\",";
         json += "\"domains\":\"" + escape(Utf8_(subscription.domains)) + "\",";
         json += "\"addresses\":\"" + escape(Utf8_(subscription.addresses)) + "\",";
         json += "\"enabled\":" + AnsiString(subscription.enabled ? "true" : "false") + ",";
         json += "\"include_subject\":" + AnsiString(subscription.include_subject ? "true" : "false") + ",";

         AnsiString numbers;
         numbers.Format("\"max_per_minute\":%d,\"created\":%I64d,\"pending\":%d,\"delivered\":%d,\"dead_lettered\":%d,\"dropped\":%d",
            subscription.max_per_minute, subscription.created, pending, delivered, deadLettered,
            WebhookManager::Instance()->DroppedSinceStart(subscription.name));
         json += numbers;

         json += "}";

         return json;
      };

      switch (route.kind)
      {
      case RouteWebhookList:
         {
            std::vector<WebhookManager::Subscription> subscriptions;

            if (!WebhookManager::Instance()->GetSubscriptions(subscriptions))
               return BuildResponse_(500, "{\"error\":\"the webhook subscriptions could not be read\"}");

            AnsiString body = "{\"webhooks\":[";

            for (size_t i = 0; i < subscriptions.size(); i++)
            {
               if (i > 0)
                  body += ",";

               body += subscriptionJson(subscriptions[i]);
            }

            body += "],\"events\":[";

            // The event names this build raises, in the answer rather than only in
            // the documentation: a client that offers a choice of events should
            // offer the ones the server it is talking to actually has.
            const std::vector<String> known = WebhookManager::KnownEvents();

            for (size_t i = 0; i < known.size(); i++)
            {
               if (i > 0)
                  body += ",";

               body += "\"" + escape(Utf8_(known[i])) + "\"";
            }

            body += "]}";

            return BuildResponse_(200, body);
         }

      case RouteWebhookGet:
         {
            WebhookManager::Subscription subscription;

            if (!WebhookManager::Instance()->GetSubscription(String(route.identifier), subscription))
               return BuildResponse_(404, "{\"error\":\"no such webhook subscription\"}");

            return BuildResponse_(200, subscriptionJson(subscription));
         }

      case RouteWebhookCreate:
      case RouteWebhookUpdate:
         {
            const bool creating = route.kind == RouteWebhookCreate;

            JsonValue body;
            if (!ParseOptionalObject(requestBody, body))
               return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

            AnsiString unknown;
            if (!OnlyKnownKeys(body, unknown))
               return BuildResponse_(400, "{\"error\":\"unknown field: " + escape(unknown) + "\"}");

            WebhookManager::Subscription subscription;

            if (creating)
            {
               const JsonValue *name = body.Get("name");
               if (name == nullptr || !name->IsString())
                  return BuildResponse_(400, "{\"error\":\"name the subscription\"}");

               subscription.name = Utf8ToString(name->AsString());
            }
            else
            {
               // A field the body leaves out keeps the value it has, so a client
               // that wants to switch one subscription off does not have to send
               // back every other field - and cannot blank a secret by forgetting
               // it. The name is the path's and is never changed by a body.
               if (!WebhookManager::Instance()->GetSubscription(String(route.identifier), subscription))
                  return BuildResponse_(404, "{\"error\":\"no such webhook subscription\"}");

               if (body.Get("name") != nullptr)
                  return BuildResponse_(400, "{\"error\":\"a subscription's name is the one in the path; create another to rename it\"}");
            }

            const JsonValue *member = nullptr;

            if ((member = body.Get("url")) != nullptr)
            {
               if (!member->IsString())
                  return BuildResponse_(400, "{\"error\":\"url is a string\"}");

               subscription.url = Utf8ToString(member->AsString());
            }

            // Write-only, as a secret should be: it can be set and replaced, and it
            // is never handed back. An empty string keeps what is there rather than
            // clearing it, because a subscription with no secret is refused anyway
            // and a client that sent secret:"" by accident would otherwise break a
            // working endpoint.
            if ((member = body.Get("secret")) != nullptr && member->IsString())
            {
               const String given = Utf8ToString(member->AsString());

               if (!given.IsEmpty())
                  subscription.secret = given;
            }

            AnsiString listError;

            if (!ReadList(body, "events", subscription.events, listError) ||
                !ReadList(body, "domains", subscription.domains, listError) ||
                !ReadList(body, "addresses", subscription.addresses, listError))
               return BuildResponse_(400, "{\"error\":\"" + escape(listError) + "\"}");

            if ((member = body.Get("enabled")) != nullptr)
            {
               if (!member->IsBool())
                  return BuildResponse_(400, "{\"error\":\"enabled is true or false\"}");

               subscription.enabled = member->AsBool();
            }

            if ((member = body.Get("include_subject")) != nullptr)
            {
               if (!member->IsBool())
                  return BuildResponse_(400, "{\"error\":\"include_subject is true or false\"}");

               subscription.include_subject = member->AsBool();
            }

            if ((member = body.Get("max_per_minute")) != nullptr)
            {
               if (!member->IsNumber())
                  return BuildResponse_(400, "{\"error\":\"max_per_minute is a whole number\"}");

               subscription.max_per_minute = (int) member->AsInt64();
            }

            String refusal;
            if (!WebhookManager::Instance()->SaveSubscription(subscription, creating, refusal))
            {
               AnsiString error;
               error.Format("{\"error\":\"%hs\"}", escape(Utf8_(refusal)).c_str());

               // A name that is taken is a conflict rather than a bad request: the
               // body was fine, the server already has one.
               return BuildResponse_(creating && refusal.Find(_T("exists already")) >= 0 ? 409 : 400, error);
            }

            String saved = creating ? subscription.name : String(route.identifier);
            saved.MakeLower();

            LOG_APPLICATION("RestApi: Webhook subscription '" + saved + "' " +
               String(creating ? _T("created") : _T("changed")) + " by " + String(caller.identity) + ".");

            WebhookManager::Subscription stored;
            if (!WebhookManager::Instance()->GetSubscription(saved, stored))
               return BuildResponse_(500, "{\"error\":\"the subscription was saved but could not be read back\"}");

            return BuildResponse_(creating ? 201 : 200, subscriptionJson(stored));
         }

      case RouteWebhookDelete:
         {
            if (!WebhookManager::Instance()->DeleteSubscription(String(route.identifier)))
               return BuildResponse_(404, "{\"error\":\"no such webhook subscription\"}");

            LOG_APPLICATION("RestApi: Webhook subscription '" + String(route.identifier) + "' deleted by " +
               String(caller.identity) + ".");

            return BuildResponse_(200, "{\"deleted\":true}");
         }

      case RouteWebhookDeliveryList:
         {
            const AnsiString name = QueryParameter_(route.query, "webhook");
            const AnsiString stateWord = QueryParameter_(route.query, "state");

            if (!stateWord.IsEmpty() && StateFromWord(stateWord) == 0)
               return BuildResponse_(400, "{\"error\":\"state is pending, delivered or dead-lettered\"}");

            int limit = 100;

            const AnsiString limitText = QueryParameter_(route.query, "limit");
            if (!limitText.IsEmpty())
               limit = atoi(limitText.c_str());

            std::vector<WebhookManager::Delivery> deliveries;

            if (!WebhookManager::Instance()->GetDeliveries(String(name), StateFromWord(stateWord), limit, deliveries))
               return BuildResponse_(500, "{\"error\":\"the webhook queue could not be read\"}");

            AnsiString body = "{\"deliveries\":[";

            for (size_t i = 0; i < deliveries.size(); i++)
            {
               const WebhookManager::Delivery &delivery = deliveries[i];

               if (i > 0)
                  body += ",";

               AnsiString numbers;
               numbers.Format("{\"id\":%I64d,\"created\":%I64d,\"attempts\":%d,\"next_attempt\":%I64d,",
                  delivery.id, delivery.created, delivery.tries, delivery.next);

               body += numbers;
               body += "\"webhook\":\"" + escape(Utf8_(delivery.subscription)) + "\",";
               body += "\"event\":\"" + escape(Utf8_(delivery.event)) + "\",";
               body += "\"state\":\"" + AnsiString(StateWord(delivery.state)) + "\",";
               body += "\"error\":\"" + escape(Utf8_(delivery.error)) + "\",";

               // The body as it was sent, so that an administrator looking at a
               // dead letter can see what the receiver would have got - and so that
               // a receiver's author can reproduce the signature from it.
               body += "\"body\":\"" + escape(Utf8_(delivery.body)) + "\"}";
            }

            body += "]}";

            return BuildResponse_(200, body);
         }

      case RouteWebhookRun:
         {
            // One pass of WebhookTask, now. The scheduled one runs every minute;
            // this is for the administrator who has just configured an endpoint and
            // wants to know before the minute is out.
            const int delivered = WebhookManager::Instance()->RunDeliveries();
            const int removed = WebhookManager::Instance()->PurgeOldDeliveries();

            AnsiString body;
            body.Format("{\"delivered\":%d,\"removed\":%d}", delivered, removed);

            return BuildResponse_(200, body);
         }

      case RouteWebhookTest:
         {
            JsonValue body;
            if (!ParseOptionalObject(requestBody, body))
               return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

            WebhookManager::Event event;

            const JsonValue *member = body.Get("event");
            event.name = member != nullptr && member->IsString()
               ? Utf8ToString(member->AsString())
               : String(WebhookManager::EventMessageReceived);

            if (!WebhookManager::IsKnownEvent(event.name))
               return BuildResponse_(400, "{\"error\":\"this server raises message.received, message.delivered, message.bounced, message.quarantined and account.locked\"}");

            if ((member = body.Get("domain")) != nullptr && member->IsString())
               event.domain = Utf8ToString(member->AsString());

            if ((member = body.Get("account")) != nullptr && member->IsString())
            {
               event.account = Utf8ToString(member->AsString());
               event.recipient = event.account;
            }

            if ((member = body.Get("sender")) != nullptr && member->IsString())
               event.sender = Utf8ToString(member->AsString());

            if ((member = body.Get("subject")) != nullptr && member->IsString())
               event.subject = Utf8ToString(member->AsString());

            event.reason = _T("Raised through POST /api/v1/webhooks/test.");

            // Through exactly the path a real event takes - the same matching, the
            // same domain switches, the same ceiling - so that what arrives is what
            // would arrive. An event nothing subscribes to queues nothing and says
            // so rather than pretending.
            const int queued = WebhookManager::Instance()->Raise(event);

            AnsiString answer;
            answer.Format("{\"queued\":%d}", queued);

            return BuildResponse_(200, answer);
         }

      default:
         break;
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }

   AnsiString
   RestApiServer::OpenApiWebhooksPaths_()
   {
      static const char *paths =
         ",\"/api/v1/webhooks\":{\"get\":{\"summary\":\"The webhook subscriptions\","
         "\"description\":\"One entry per subscription: its name, the endpoint, whether a shared secret is set (the secret itself is never returned), the events, domains and addresses it is confined to (empty means every one), whether it is on, whether its bodies may carry a subject, its rate limit, and how many of its deliveries are pending, delivered, dead-lettered and - since this server started - dropped at its ceiling. events lists the event names this build raises. Refused to every API key. The administrator password, or an account whose role holds view_configuration and reaches every domain.\","
         "\"responses\":{\"200\":{\"description\":\"webhooks and events\"}}},"
         "\"post\":{\"summary\":\"Create a webhook subscription\","
         "\"description\":\"Body: name (required; letters, digits and - _ . stored lower case, since it is the identifier in this path), url (required; https to any address THIS SERVER can reach - which includes addresses only it can reach, so this is the server being pointed at a host and port of the caller's choosing - or plain http only to an address on this machine, since the body says who wrote to whom), secret (required; the shared secret the HMAC-SHA256 signature is computed under, protected at rest and never returned), events, domains and addresses (each a list of strings or one comma-separated string; empty means every one), enabled (default true), include_subject (default false - and a subject is carried only where the domain's webhook_subjects is on as well) and max_per_minute (0 for the server's WebhookMaxPerMinute). Refused to every API key. The administrator password, or an account whose role holds manage_server and reaches every domain.\","
         "\"requestBody\":{\"required\":true,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"name\",\"url\",\"secret\"],\"properties\":{"
         "\"name\":{\"type\":\"string\",\"description\":\"Letters, digits and - _ . at most 64 characters.\"},"
         "\"url\":{\"type\":\"string\",\"description\":\"https://host/path, reached from this server, or http:// only to this machine.\"},"
         "\"secret\":{\"type\":\"string\",\"writeOnly\":true,\"description\":\"Write-only. The bodies are signed under it; an empty string in a change keeps the one that is set.\"},"
         "\"events\":{\"type\":\"string\",\"description\":\"message.received, message.delivered, message.bounced, message.quarantined, account.locked - comma-separated. Empty: every event.\"},"
         "\"domains\":{\"type\":\"string\",\"description\":\"Comma-separated domain names. Empty: every domain.\"},"
         "\"addresses\":{\"type\":\"string\",\"description\":\"Comma-separated addresses. Empty: every address.\"},"
         "\"enabled\":{\"type\":\"boolean\"},"
         "\"include_subject\":{\"type\":\"boolean\",\"description\":\"Carry the message subject on message.received and message.quarantined. Only where the domain allows it too.\"},"
         "\"max_per_minute\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100000,\"description\":\"Events past this in one minute are dropped and counted, not queued. 0: the server's WebhookMaxPerMinute.\"}"
         "}}}}},"
         "\"responses\":{\"201\":{\"description\":\"The subscription as it now stands\"},\"400\":{\"description\":\"A field was refused, with the reason\"},\"409\":{\"description\":\"A subscription of that name exists\"}}}}"
         ",\"/api/v1/webhooks/{name}\":{\"get\":{\"summary\":\"One webhook subscription\","
         "\"description\":\"As the listing shows it. The shared secret is never returned.\","
         "\"parameters\":[{\"name\":\"name\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"The subscription\"},\"404\":{\"description\":\"No such subscription\"}}},"
         "\"put\":{\"summary\":\"Change a webhook subscription\","
         "\"description\":\"Any subset of url, secret, events, domains, addresses, enabled, include_subject and max_per_minute. A field left out keeps its value, and an empty secret keeps the one that is set rather than clearing it - a subscription with no secret would send bodies nobody can verify and is refused. The name is the path's. Refused to every API key. The administrator password, or an account whose role holds manage_server and reaches every domain, since a url is an address this server will connect to.\","
         "\"parameters\":[{\"name\":\"name\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"requestBody\":{\"required\":false,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{"
         "\"url\":{\"type\":\"string\"},\"secret\":{\"type\":\"string\",\"writeOnly\":true},\"events\":{\"type\":\"string\"},\"domains\":{\"type\":\"string\"},"
         "\"addresses\":{\"type\":\"string\"},\"enabled\":{\"type\":\"boolean\"},\"include_subject\":{\"type\":\"boolean\"},"
         "\"max_per_minute\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100000}}}}}},"
         "\"responses\":{\"200\":{\"description\":\"The subscription as saved\"},\"400\":{\"description\":\"A field was refused, with the reason\"},\"404\":{\"description\":\"No such subscription\"}}},"
         "\"delete\":{\"summary\":\"Delete a webhook subscription\","
         "\"description\":\"Removes it and everything of its that is still queued, since a delivery to an endpoint that no longer exists is a request nobody wants made. Refused to every API key. The administrator password, or an account whose role holds manage_settings and reaches every domain.\","
         "\"parameters\":[{\"name\":\"name\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"Deleted\"},\"404\":{\"description\":\"No such subscription\"}}}}"
         ",\"/api/v1/webhooks/deliveries\":{\"get\":{\"summary\":\"The webhook delivery queue\","
         "\"description\":\"Newest first: the queue row's id (the same value the X-hMailServer-Delivery header carries, which is how a receiver recognises a retry), when it was raised, which subscription and which event, how many attempts have been made, when the next is due, the last failure, and the body exactly as it was sent - which is what an administrator reads when a delivery has been dead-lettered. Query: webhook, state (pending, delivered or dead-lettered) and limit (1 to 1000, default 100). Refused to every API key. The administrator password, or an account whose role holds view_configuration and reaches every domain.\","
         "\"parameters\":["
         "{\"name\":\"webhook\",\"in\":\"query\",\"schema\":{\"type\":\"string\"}},"
         "{\"name\":\"state\",\"in\":\"query\",\"schema\":{\"type\":\"string\",\"enum\":[\"pending\",\"delivered\",\"dead-lettered\"]}},"
         "{\"name\":\"limit\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"}}"
         "],\"responses\":{\"200\":{\"description\":\"The deliveries\"},\"400\":{\"description\":\"state is not one of the three words\"}}}}"
         ",\"/api/v1/webhooks/run\":{\"post\":{\"summary\":\"Run one webhook delivery pass now\","
         "\"description\":\"Attempts every delivery that is due and removes what WebhookRetentionDays no longer keeps - the same pass the scheduled task makes every minute. One pass runs at a time: if the scheduled pass is running this waits for it and then makes its own, so delivered counts what THIS pass sent, which may be nothing. Refused to every API key. The administrator password, or an account whose role holds manage_server and reaches every domain.\","
         "\"responses\":{\"200\":{\"description\":\"delivered and removed\"}}}}"
         ",\"/api/v1/webhooks/test\":{\"post\":{\"summary\":\"Raise a webhook event on request\","
         "\"description\":\"Raises the named event through exactly the path a real one takes - the same matching, the same domain switches, the same rate ceiling - so that what arrives is what would arrive. Body: event (one of the five; message.received by default), domain, account, sender and subject. Answers queued:0 when nothing subscribes to it, the domain has webhooks switched off, or every matching subscription is at its ceiling. Refused to every API key. The administrator password, or an account whose role holds manage_server and reaches every domain, since it makes the server connect to every matching endpoint.\","
         "\"requestBody\":{\"required\":false,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{"
         "\"event\":{\"type\":\"string\",\"enum\":[\"message.received\",\"message.delivered\",\"message.bounced\",\"message.quarantined\",\"account.locked\"]},"
         "\"domain\":{\"type\":\"string\"},\"account\":{\"type\":\"string\"},\"sender\":{\"type\":\"string\"},\"subject\":{\"type\":\"string\"}}}}}},"
         "\"responses\":{\"200\":{\"description\":\"How many subscriptions the event was queued for\"},\"400\":{\"description\":\"Not an event this server raises\"}}}}";

      return AnsiString(paths);
   }
}
