// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The signed-in account's Web Push devices: GET /api/v1/me/push, POST
// /api/v1/me/push/subscriptions, PUT and DELETE
// /api/v1/me/push/subscriptions/{id}, POST /api/v1/me/push/test.
//
// A subscription is made in the browser and posted here; the rows and what each
// device is allowed to be told are PushSubscriptions', and the sending is
// PushNotifier's. This file is the surface between them and it has one rule of
// its own, which is the reason the subscription's keys are never in any answer:
//
//   the endpoint, the p256dh key and the auth secret together are the capability
//   to send a notification to that device under this server's VAPID key. A
//   listing that echoed them back would put that capability into every log, every
//   browser cache and every configuration export that ever touched this route.
//
// So a listing gives the id, the device's name, when it was made, when something
// last reached it, whether it is still being tried, and the person's own choices.
// What it never gives is anything that could be used to send. The same rule that
// IniSettingStore::IsSecretName keeps for a setting and that the relay password
// keeps for a domain, kept here by construction rather than by a name test: there
// is no code path that emits them.
//
// The administrator's half of the feature - the master switch, the VAPID key pair
// and the limits - is the "push" settings group in RestApiSettings.cpp.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "JsonDocument.h"
#include "HttpsClient.h"
#include "PushNotifier.h"
#include "PushSubscriptions.h"
#include "Unicode.h"
#include "WebPush.h"
#include "../Application/Configuration.h"
#include "../Application/Logger.h"
#include "../BO/Account.h"
#include "../Persistence/PersistentAccount.h"
#include "AuditTrail.h"
#include "Parsing/StringParser.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The listener already refuses a body above 64 KiB; this is the same figure,
      // stated here, and a subscription is a few hundred bytes.
      const size_t PushBodyMaximum = 8 * 1024;

      AnsiString Int64Text(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      AnsiString NumberText(long value)
      {
         AnsiString text;
         text.Format("%d", value);
         return text;
      }

      String WideOf(const std::string &value)
      {
         String text;
         Unicode::MultiByteToWide(AnsiString(value.c_str()), text);
         return text;
      }

      // A member of the body as a string, or the empty string when it is absent or
      // is not one. The caller decides whether absent is acceptable.
      String MemberText(const JsonValue &document, const char *name)
      {
         const JsonValue *member = document.Get(name);
         if (member == nullptr || !member->IsString())
            return String();

         return WideOf(member->AsString());
      }

      // A member as a whole number. absent is what comes back when the member is
      // not there, so that "leave this as it was" and "set this to zero" are
      // different requests.
      int MemberNumber(const JsonValue &document, const char *name, int absent)
      {
         const JsonValue *member = document.Get(name);
         if (member == nullptr || !member->IsNumber())
            return absent;

         return (int) member->AsNumber();
      }
   }

   /*
      One device. Everything a person needs to recognise it and to decide what it
      should tell them - and nothing that could be used to send to it.
   */
   AnsiString
   RestApiServer::PushSubscriptionJson_(const PushSubscriptions::Subscription &subscription)
   {
      AnsiString json = "{\"id\":" + Int64Text(subscription.id);
      json += ",\"device\":\"" + JsonEscape_(Utf8_(subscription.device)) + "\"";
      json += ",\"created\":" + Int64Text(subscription.created);
      json += ",\"last_sent\":" + Int64Text(subscription.last_sent);
      json += ",\"state\":\"" + AnsiString(subscription.state == PushSubscriptions::StateDeadLettered ? "dead-lettered" : "active") + "\"";
      json += ",\"failures\":" + NumberText(subscription.failures);
      json += ",\"next_attempt\":" + Int64Text(subscription.next_attempt);
      json += ",\"last_detail\":\"" + JsonEscape_(Utf8_(subscription.last_detail)) + "\"";
      json += ",\"detail\":" + NumberText(subscription.detail);
      json += ",\"senders\":\"" + JsonEscape_(Utf8_(subscription.senders)) + "\"";
      json += ",\"labels\":\"" + JsonEscape_(Utf8_(subscription.labels)) + "\"";
      json += ",\"quiet_from\":" + NumberText(subscription.quiet_from);
      json += ",\"quiet_to\":" + NumberText(subscription.quiet_to);
      json += ",\"quiet_offset\":" + NumberText(subscription.quiet_offset);

      // The endpoint's host, and not the endpoint: a person recognising which
      // browser a row belongs to is helped by "updates.push.services.mozilla.com"
      // and is not helped by the token after it, which is the capability.
      AnsiString host;
      AnsiString audience;
      if (WebPush::AudienceOf(AnsiString(subscription.endpoint), audience))
         host = audience;

      json += ",\"service\":\"" + JsonEscape_(host) + "\"";
      json += "}";
      return json;
   }

   AnsiString
   RestApiServer::PushStateJson_(__int64 accountId)
   {
      std::vector<PushSubscriptions::Subscription> subscriptions;
      PushSubscriptions::ForAccount(accountId, subscriptions);

      AnsiString json = "{";
      json += "\"enabled\":";
      json += Configuration::Instance()->GetWebPushEnabled() ? "true" : "false";

      // The public key, which a page needs to subscribe at all. A server with the
      // feature on and no key pair is a misconfiguration, and the page has to be
      // able to say so rather than failing inside pushManager.subscribe.
      json += ",\"vapid_public_key\":\"" + JsonEscape_(Utf8_(Configuration::Instance()->GetWebPushVapidPublicKey())) + "\"";
      json += ",\"max_devices\":" + NumberText(Configuration::Instance()->GetWebPushMaxPerAccount());
      json += ",\"subscriptions\":[";

      for (size_t i = 0; i < subscriptions.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += PushSubscriptionJson_(subscriptions[i]);
      }

      json += "]}";
      return json;
   }

   HttpResponse
   RestApiServer::HandleMePush_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      return BuildResponse_(200, PushStateJson_(account->GetID()));
   }

   HttpResponse
   RestApiServer::HandleMePushSubscribe_(const Caller &caller, const AnsiString &requestBody, bool fromThisMachine)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      if (!Configuration::Instance()->GetWebPushEnabled())
         return BuildResponse_(409, "{\"error\":\"Web Push is switched off on this server\"}");

      if (Configuration::Instance()->GetWebPushVapidPublicKey().IsEmpty())
         return BuildResponse_(409, "{\"error\":\"this server has no Web Push key pair, so a subscription made now could never be sent to\"}");

      if (requestBody.size() > PushBodyMaximum)
         return BuildResponse_(413, "{\"error\":\"the body is too large\"}");

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.size()), document, parseError) || !document.IsObject())
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

      PushSubscriptions::Subscription subscription;
      subscription.account_id = account->GetID();
      subscription.endpoint = MemberText(document, "endpoint");
      subscription.device = MemberText(document, "device");

      // The browser hands the page {endpoint, keys: {p256dh, auth}} and that is
      // what the page sends, unchanged; the two keys are also accepted at the top
      // level so that a client which flattened the object is not refused for a
      // shape that carries the same thing.
      const JsonValue *keys = document.Get("keys");
      if (keys != nullptr && keys->IsObject())
      {
         subscription.p256dh = MemberText(*keys, "p256dh");
         subscription.auth = MemberText(*keys, "auth");
      }
      else
      {
         subscription.p256dh = MemberText(document, "p256dh");
         subscription.auth = MemberText(document, "auth");
      }

      subscription.detail = MemberNumber(document, "detail", PushSubscriptions::DetailSender);
      subscription.senders = MemberText(document, "senders");
      subscription.labels = MemberText(document, "labels");
      subscription.quiet_from = MemberNumber(document, "quiet_from", -1);
      subscription.quiet_to = MemberNumber(document, "quiet_to", -1);
      subscription.quiet_offset = MemberNumber(document, "quiet_offset", 0);

      // The ceiling is counted before the save and against the rows that exist, so
      // a device re-subscribing under a new endpoint cannot be refused by its own
      // old row - Save replaces a row with the same endpoint, so only a genuinely
      // new device reaches the limit.
      String existingHash = PushSubscriptions::HashOf(subscription.endpoint);
      std::vector<PushSubscriptions::Subscription> existing;
      PushSubscriptions::ForAccount(account->GetID(), existing);

      bool replacing = false;
      for (const PushSubscriptions::Subscription &one : existing)
      {
         if (one.endpoint_hash == existingHash)
            replacing = true;
      }

      if (!replacing && (int) existing.size() >= Configuration::Instance()->GetWebPushMaxPerAccount())
         return BuildResponse_(400, "{\"error\":\"this account already has as many devices as the server allows; remove one first\"}");

      String refusal;
      if (!PushSubscriptions::Save(subscription, refusal, fromThisMachine))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(Utf8_(refusal)) + "\"}");

      LOG_APPLICATION("RestApi: " + account->GetAddress() + " turned Web Push notifications on for a device.");

      return BuildResponse_(201, PushSubscriptionJson_(subscription));
   }

   HttpResponse
   RestApiServer::HandleMePushUpdate_(const Caller &caller, __int64 id, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      if (requestBody.size() > PushBodyMaximum)
         return BuildResponse_(413, "{\"error\":\"the body is too large\"}");

      PushSubscriptions::Subscription subscription;
      if (!PushSubscriptions::ById(account->GetID(), id, subscription))
         return BuildResponse_(404, "{\"error\":\"no such device of this account\"}");

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.size()), document, parseError) || !document.IsObject())
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

      // Each member left out keeps the value it had, so a page that changes the
      // quiet hours does not have to send the sender list back with them.
      int detail = MemberNumber(document, "detail", subscription.detail);
      const JsonValue *senders = document.Get("senders");
      const JsonValue *labels = document.Get("labels");

      String refusal;
      if (!PushSubscriptions::SaveChoice(account->GetID(), id, detail,
                                         senders != nullptr && senders->IsString() ? MemberText(document, "senders") : subscription.senders,
                                         labels != nullptr && labels->IsString() ? MemberText(document, "labels") : subscription.labels,
                                         MemberNumber(document, "quiet_from", subscription.quiet_from),
                                         MemberNumber(document, "quiet_to", subscription.quiet_to),
                                         MemberNumber(document, "quiet_offset", subscription.quiet_offset),
                                         refusal))
         return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(Utf8_(refusal)) + "\"}");

      PushSubscriptions::ById(account->GetID(), id, subscription);
      return BuildResponse_(200, PushSubscriptionJson_(subscription));
   }

   HttpResponse
   RestApiServer::HandleMePushDelete_(const Caller &caller, __int64 id)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      PushSubscriptions::Subscription subscription;
      if (!PushSubscriptions::ById(account->GetID(), id, subscription))
         return BuildResponse_(404, "{\"error\":\"no such device of this account\"}");

      if (!PushSubscriptions::Remove(account->GetID(), id))
         return BuildResponse_(500, "{\"error\":\"the device could not be removed\"}");

      LOG_APPLICATION("RestApi: " + account->GetAddress() + " turned Web Push notifications off for a device.");

      return BuildResponse_(200, "{\"removed\":true}");
   }

   /*
      One push to one device, now. What this is for is the moment before a person
      relies on a notification: a browser can grant permission, hand over a
      subscription and still never show anything, and the only way to find that out
      is to send one. Neither the quiet hours nor the sender list applies, because
      this is not mail arriving - it is a person pressing a button.
   */
   HttpResponse
   RestApiServer::HandleMePushTest_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      if (requestBody.size() > PushBodyMaximum)
         return BuildResponse_(413, "{\"error\":\"the body is too large\"}");

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.size()), document, parseError) || !document.IsObject())
         return BuildResponse_(400, "{\"error\":\"the body must be a JSON object naming the device\"}");

      __int64 id = (__int64) MemberNumber(document, "id", 0);
      if (id <= 0)
         return BuildResponse_(400, "{\"error\":\"name the device with id\"}");

      PushSubscriptions::Subscription subscription;
      if (!PushSubscriptions::ById(account->GetID(), id, subscription))
         return BuildResponse_(404, "{\"error\":\"no such device of this account\"}");

      // The push service is another server, on one of the REST workers: it
      // waits its turn with the other requests that reach out (ReachingOut_).
      ReachingOut_ turn(caller);
      if (!turn.Admitted())
         return BuildBusyReachingOutResponse_();

      String error;
      if (!PushNotifier::Instance()->SendTest(subscription, error))
         return BuildResponse_(502, "{\"error\":\"" + JsonEscape_(Utf8_(error)) + "\"}");

      return BuildResponse_(200, "{\"sent\":true}");
   }

   /*
      The administrator's two: how many devices an account is being notified on,
      with the same safe listing the account itself gets, and every one of them
      stopped. The second is what a lost phone needs - the subscription is the
      capability to notify that device, and the person cannot remove it from a
      device they no longer have. Authorize_ has already confined the address to
      the credential's domains, as it does for a password reset.
   */
   HttpResponse
   RestApiServer::HandleAccountPush_(const Caller &caller, const AnsiString &address)
   {
      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(account, String(address)) || account->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      std::vector<PushSubscriptions::Subscription> subscriptions;
      PushSubscriptions::ForAccount(account->GetID(), subscriptions);

      AnsiString json = "{\"count\":" + NumberText((long) subscriptions.size()) + ",\"subscriptions\":[";
      for (size_t i = 0; i < subscriptions.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += PushSubscriptionJson_(subscriptions[i]);
      }
      json += "]}";

      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleAccountPushRemove_(const Caller &caller, const AnsiString &address)
   {
      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(account, String(address)) || account->GetID() == 0)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      std::vector<PushSubscriptions::Subscription> subscriptions;
      PushSubscriptions::ForAccount(account->GetID(), subscriptions);

      long removed = 0;
      for (const PushSubscriptions::Subscription &subscription : subscriptions)
      {
         if (PushSubscriptions::Remove(account->GetID(), subscription.id))
            removed++;
      }

      if (removed > 0)
      {
         // The rows are the person's own and their table is not audited; the
         // administrator's act is, as an event under the actor the dispatch
         // installed for this request. InterfaceAccount::RemovePushSubscriptions
         // records the same.
         AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("push-subscriptions-removed"),
            Formatter::Format(_T("Web Push notifications were stopped on all {0} device(s)"), (int) removed));

         LOG_APPLICATION("RestApi: Web Push notifications were stopped on all " + StringParser::IntToString((int) removed) +
                         " device(s) of " + account->GetAddress() + " by " + String(caller.identity) + ".");
      }

      return BuildResponse_(200, "{\"removed\":" + NumberText(removed) + "}");
   }
}
