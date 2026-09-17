// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The REST API's compromised-account review. See RestApiServer.h and
// Common/Util/CompromiseDetector.h.
//
//    GET    /api/v1/review                            what is held for review, newest first
//    POST   /api/v1/review/{id}/release               back into the delivery queue
//    DELETE /api/v1/review/{id}                       gone
//    GET    /api/v1/accounts/{address}/review         the account's review state and baseline
//    PUT    /api/v1/accounts/{address}/review         exempt, sensitivity
//    POST   /api/v1/accounts/{address}/review/unlock  the compromise lock alone, cleared
//
// The thresholds and the mode are not here: they are the compromise_* keys of
// PUT /api/v1/settings, so they travel with the rest of the configuration.
//
// The held list has the delivery queue's shape - one entry names a sender and
// recipients in any number of domains, and releasing one delivers mail - so it
// takes the queue's permissions and is refused to a credential confined to
// named domains. An account's review state is that account's, scoped to its
// domain like its other reads and writes. All six are refused to an API key:
// a key that could release held mail could send what a takeover of its own
// account got held, and one that could mark an account exempt could switch
// the detection off for the account it stole.
//
// Every change here is written by a thread with no actor on it or by a delete
// the statement chokepoint does not record (the tables are traffic), so each
// is recorded with RecordEvent under the actor the dispatch installed.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "JsonDocument.h"
#include "CompromiseDetector.h"
#include "AuditTrail.h"
#include "../Application/Configuration.h"
#include "../BO/Account.h"
#include "../Persistence/PersistentAccount.h"
#include "../Util/Parsing/StringParser.h"

#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const char *ModeWord(int mode)
      {
         if (mode == CompromiseDetector::ModeEnforce)
            return "enforce";
         if (mode == CompromiseDetector::ModeWatch)
            return "watch";
         return "off";
      }

      const char *SensitivityWord(int sensitivity)
      {
         if (sensitivity == CompromiseDetector::SensitivityRelaxed)
            return "relaxed";
         if (sensitivity == CompromiseDetector::SensitivityStrict)
            return "strict";
         return "default";
      }

      bool SensitivityFromWord(const std::string &word, int &sensitivity)
      {
         if (word == "default") { sensitivity = CompromiseDetector::SensitivityDefault; return true; }
         if (word == "relaxed") { sensitivity = CompromiseDetector::SensitivityRelaxed; return true; }
         if (word == "strict") { sensitivity = CompromiseDetector::SensitivityStrict; return true; }
         return false;
      }

      const char *SourceWord(int source)
      {
         return source == CompromiseDetector::SourceWebmail ? "webmail" : "smtp";
      }

      // The account an address names, or nothing. As RestApiRoles.cpp finds
      // one: the answer never says which of "no such account" and "not yours"
      // it was, since DomainNamedByRoute_ has already decided the second.
      std::shared_ptr<Account> ReviewedAccount(const AnsiString &address)
      {
         std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
         if (!PersistentAccount::ReadObject(account, String(address)) || account->GetID() == 0)
            return std::shared_ptr<Account>();
         return account;
      }

      AnsiString HeldJson(const CompromiseDetector::Held &held)
      {
         AnsiString numbers;
         numbers.Format("{\"id\":%I64d,\"message_id\":%I64d,\"size\":%I64d,\"held\":%I64d,\"source\":\"%hs\",",
            held.id, held.message_id, held.size, held.held, SourceWord(held.source));
         AnsiString json = numbers;
         json += "\"account\":\"" + RestApiServer::JsonEscapeUtf8(held.account) + "\"";
         json += ",\"sender\":\"" + RestApiServer::JsonEscapeUtf8(held.sender) + "\"";
         json += ",\"recipients\":\"" + RestApiServer::JsonEscapeUtf8(held.recipients) + "\"";
         json += ",\"subject\":\"" + RestApiServer::JsonEscapeUtf8(held.subject) + "\"";
         json += ",\"reason\":\"" + RestApiServer::JsonEscapeUtf8(held.reason) + "\"}";
         return json;
      }

      // As a String throughout: a subject or an address outside ASCII is
      // recorded as it is, not as the question marks a narrowing would leave.
      String HeldDetail(const CompromiseDetector::Held &held)
      {
         return _T("from ") + held.sender + _T(" to ") + held.recipients + _T(", subject \"") + held.subject + _T("\", held for: ") + held.reason;
      }
   }

   HttpResponse
   RestApiServer::HandleAccountReview_(const Caller &caller, const Route &route, const AnsiString &requestBody)
   {
      if (route.kind == RouteReviewList)
      {
         std::vector<CompromiseDetector::Held> held;
         if (!CompromiseDetector::ListHeld(held))
            return BuildResponse_(500, "{\"error\":\"the held messages could not be read\"}");

         int heldDays = Configuration::Instance()->GetCompromiseHeldDays();
         if (heldDays < 1) heldDays = 1;
         if (heldDays > 90) heldDays = 90;

         AnsiString body;
         body.Format("{\"mode\":\"%hs\",\"held_days\":%d,\"messages\":[", ModeWord(CompromiseDetector::Mode()), heldDays);
         for (size_t i = 0; i < held.size(); i++)
            body += (i ? "," : "") + HeldJson(held[i]);
         body += "]}";
         return BuildResponse_(200, body);
      }

      if (route.kind == RouteReviewRelease || route.kind == RouteReviewDiscard)
      {
         CompromiseDetector::Held held;
         if (!CompromiseDetector::ReadHeld(route.record_id, held))
            return BuildResponse_(404, "{\"error\":\"no such held message\"}");

         const bool release = route.kind == RouteReviewRelease;
         String problem;
         const bool done = release ? CompromiseDetector::Release(route.record_id, problem) : CompromiseDetector::Discard(route.record_id, problem);
         if (!done)
         {
            const int status = problem == _T("not found") || problem == _T("the held message is gone") ? 404 : 500;
            return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(Utf8_(problem)) + "\"}");
         }

         // Neither is a row the statement chokepoint records - hm_heldmessages
         // is traffic - and releasing one sends mail somebody else decided not
         // to; it is recorded here, under the actor the dispatch installed.
         const String detail = HeldDetail(held);
         if (release)
            AuditTrail::Instance()->RecordEvent(_T("held message"), StringParser::IntToString(held.message_id), _T("released"), detail);
         else
            AuditTrail::Instance()->RecordEvent(_T("held message"), StringParser::IntToString(held.message_id), _T("discarded"), detail);

         LOG_APPLICATION("RestApi: the message " + StringParser::IntToString(held.message_id) + " held for review from " + held.account +
                         (release ? " was released to the delivery queue by " : " was discarded by ") + String(caller.identity) + ".");

         return BuildResponse_(200, release ? "{\"released\":true}" : "{\"discarded\":true}");
      }

      // The rest name an account.
      std::shared_ptr<Account> account = ReviewedAccount(route.identifier);
      if (!account)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      if (route.kind == RouteAccountReviewGet)
      {
         CompromiseDetector::Review review;
         CompromiseDetector::Baseline baseline;
         if (!CompromiseDetector::ReadReview(account->GetID(), review) || !CompromiseDetector::ReadBaseline(account->GetID(), baseline))
            return BuildResponse_(500, "{\"error\":\"the account's review state could not be read\"}");

         AnsiString body = "{\"address\":\"" + JsonEscape_(Utf8_(account->GetAddress())) + "\"";
         AnsiString state;
         state.Format(",\"mode\":\"%hs\",\"exempt\":%hs,\"sensitivity\":\"%hs\",\"locked\":%hs,\"locked_at\":%I64d,\"unlocked_at\":%I64d,\"alerted_at\":%I64d,\"score\":%d,",
            ModeWord(CompromiseDetector::Mode()), review.exempt ? "true" : "false", SensitivityWord(review.sensitivity),
            review.locked ? "true" : "false", review.locked_at, review.unlocked_at, review.alerted_at, review.score);
         body += state;
         body += "\"signals\":\"" + JsonEscape_(Utf8_(review.signals)) + "\"";
         body += ",\"reason\":\"" + JsonEscape_(Utf8_(review.reason)) + "\"";

         AnsiString counts;
         counts.Format(",\"baseline\":{\"days\":%d,\"history\":%d,\"minimum_history\":%d,\"has_baseline\":%hs,\"peak_hour\":%d,\"last_hour\":%d,\"recipients_known\":%d,\"held\":%d,\"hours\":[",
            baseline.days, baseline.history, baseline.minimum_history, baseline.history >= baseline.minimum_history ? "true" : "false",
            baseline.peak_hour, baseline.last_hour, baseline.recipients_known, baseline.held);
         body += counts;
         for (size_t i = 0; i < baseline.hours.size(); i++)
         {
            AnsiString hour;
            hour.Format("%hs%d", i ? "," : "", baseline.hours[i]);
            body += hour;
         }
         body += "],\"networks\":[";
         for (size_t i = 0; i < baseline.networks.size(); i++)
            body += AnsiString(i ? "," : "") + "\"" + JsonEscape_(Utf8_(baseline.networks[i])) + "\"";
         body += "]}}";
         return BuildResponse_(200, body);
      }

      if (route.kind == RouteAccountReviewPut)
      {
         AnsiString trimmed = requestBody;
         trimmed.Trim();
         if (trimmed.IsEmpty())
            return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

         JsonValue body;
         std::string parseError;
         if (!JsonValue::Parse(std::string(trimmed.c_str(), (size_t) trimmed.GetLength()), body, parseError) || !body.IsObject())
            return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

         CompromiseDetector::Review review;
         if (!CompromiseDetector::ReadReview(account->GetID(), review))
            return BuildResponse_(500, "{\"error\":\"the account's review state could not be read\"}");

         bool exempt = review.exempt;
         int sensitivity = review.sensitivity;
         bool any = false;

         // Everything checked before anything is applied; a field left out
         // keeps its value, and an unknown one is refused by name.
         for (const std::pair<std::string, JsonValue> &member : body.Members())
         {
            if (member.first == "exempt")
            {
               if (!member.second.IsBool())
                  return BuildResponse_(400, "{\"error\":\"exempt must be true or false\"}");
               exempt = member.second.AsBool();
               any = true;
            }
            else if (member.first == "sensitivity")
            {
               if (!member.second.IsString() || !SensitivityFromWord(member.second.AsString(), sensitivity))
                  return BuildResponse_(400, "{\"error\":\"sensitivity is default, relaxed or strict\"}");
               any = true;
            }
            else
            {
               const AnsiString unknown = AnsiString(member.first.substr(0, 48).c_str());
               return BuildResponse_(400, "{\"error\":\"unknown field: " + JsonEscape_(unknown) + "\"}");
            }
         }

         if (!any)
            return BuildResponse_(400, "{\"error\":\"nothing to change: exempt or sensitivity\"}");

         if (!CompromiseDetector::WriteReviewChoices(account->GetID(), exempt, sensitivity))
            return BuildResponse_(500, "{\"error\":\"the account's review state could not be written\"}");

         // hm_accountreview is written by the submission thread with no actor
         // for the lock and the shape; the administrator's two choices are
         // recorded here, under the actor the dispatch installed.
         String detail;
         detail.Format(_T("exempt from the compromised-account detection: %s; sensitivity: %s"),
            exempt ? _T("yes") : _T("no"), String(SensitivityWord(sensitivity)).c_str());
         AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("review-changed"), detail);

         LOG_APPLICATION("RestApi: the review state of " + account->GetAddress() + " was set by " + String(caller.identity) + ": " + detail);

         AnsiString answer;
         answer.Format("{\"exempt\":%hs,\"sensitivity\":\"%hs\"}", exempt ? "true" : "false", SensitivityWord(sensitivity));
         return BuildResponse_(200, answer);
      }

      if (route.kind == RouteAccountReviewUnlock)
      {
         CompromiseDetector::Review review;
         if (!CompromiseDetector::ReadReview(account->GetID(), review))
            return BuildResponse_(500, "{\"error\":\"the account's review state could not be read\"}");

         CompromiseDetector::Unlock(account->GetID(), account->GetAddress());

         // The lock is a row written by the submission thread with no actor,
         // and the hold on the name is in memory; clearing it is recorded here.
         AuditTrail::Instance()->RecordEvent(_T("account"), account->GetAddress(), _T("compromise-lock-cleared"),
            review.locked ? _T("the compromise lock was cleared; what was held for review stays held") : _T("the account was not locked; nothing changed"));

         LOG_APPLICATION("RestApi: the compromise lock of " + account->GetAddress() + " was cleared by " + String(caller.identity) + ".");

         return BuildResponse_(200, review.locked ? "{\"unlocked\":true}" : "{\"unlocked\":false}");
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }

   AnsiString
   RestApiServer::OpenApiAccountReviewPaths_()
   {
      static const char *paths =
         ",\"/api/v1/review\":{\"get\":{\"summary\":\"Messages held for review by the compromised-account detection\","
         "\"description\":\"{mode,held_days,messages:[{id,message_id,account,sender,recipients,subject,size,held,source,reason}]}, newest first. mode is what a takeover shape does (off, watch or enforce - the compromise_detection_mode setting); held_days is how long a message waits for a review before the retention sweep discards it. Each message was kept out of the delivery queue because the account that submitted it did so in the shape of a takeover - a surge in volume past its own baseline, mostly to addresses it had never written to, at an hour of the day or from a client network it had never sent from - or on the account's standing lock, or because it was already queued from the same account within the hour of the shape; reason says which. held is seconds since 1970; source is smtp or webmail. Server-wide, like the delivery queue, and refused to a credential restricted to named domains for the same reason: one entry names recipients in any number of domains. Refused to an API key.\","
         "\"responses\":{\"200\":{\"description\":\"The held messages\"}}}}"
         ",\"/api/v1/review/{id}/release\":{\"post\":{\"summary\":\"Release a held message to the delivery queue\","
         "\"description\":\"The message goes back into the delivery queue as if it had just been submitted, and the deliverer is woken. A message the webmail sent (source webmail, a scheduled draft included) gets its copy in the sender's folder designated \\\\Sent now, marked read, when the account has such a folder and its quota has room: the send made none, since a copy of a message that was then discarded would say it was sent. The account it came from stays locked until it is unlocked separately (POST /api/v1/accounts/{address}/unlock clears every lock the account is behind; .../review/unlock clears this one alone). Recorded on the audit trail as the release of a held message, naming the sender, the recipients and the subject.\","
         "\"responses\":{\"200\":{\"description\":\"Released\"},\"404\":{\"description\":\"No such held message\"}}}}"
         ",\"/api/v1/review/{id}\":{\"delete\":{\"summary\":\"Discard a held message\","
         "\"description\":\"The message is removed from the queue and its file deleted; nothing is sent and nobody is told. Recorded on the audit trail.\","
         "\"responses\":{\"200\":{\"description\":\"Discarded\"},\"404\":{\"description\":\"No such held message\"}}}}"
         ",\"/api/v1/accounts/{address}/review\":{\"get\":{\"summary\":\"An account's state under the compromised-account detection\","
         "\"description\":\"{address,mode,exempt,sensitivity,locked,locked_at,unlocked_at,alerted_at,score,signals,reason,baseline:{days,history,minimum_history,has_baseline,peak_hour,last_hour,recipients_known,held,hours,networks}}. exempt: the account is never judged. sensitivity: default, relaxed (one point more is needed before the shape is a takeover) or strict (one fewer). locked, locked_at (seconds since 1970), score, signals and reason describe the last takeover shape seen: signals is which of volume, recipients, hour and address scored, reason one sentence per signal. unlocked_at is the last unlock (0 for never), which is where the account's last hour starts from: what it submitted before an administrator let it back in has been looked at and does not count towards the next hour's volume or recipients. alerted_at is the last account.compromised alert for the account (0 for never); the alert is raised once per account per hour, and always when the account is locked. baseline is what the detection judges against: the account's authenticated submissions in the last days days (history), whether that reaches minimum_history and so counts as a baseline, its busiest hour in the window (peak_hour) and the last hour (last_hour), how many distinct addresses it has written to (recipients_known), how many of its messages are held, the hours of the day (UTC) it has submitted at and the client networks (/24, or /64 for IPv6) it has submitted from.\","
         "\"responses\":{\"200\":{\"description\":\"The review state\"},\"404\":{\"description\":\"No such account\"}}},"
         "\"put\":{\"summary\":\"Set an account's exemption or sensitivity\","
         "\"description\":\"Body: any of exempt (true or false) and sensitivity (default, relaxed or strict). A field left out keeps its value; an unknown field or a wrong type is a 400 naming it. Exempting an account is how a newsletter sender, a ticketing system or a scanner that mails strangers in bulk from a new network is kept out of the detection without switching it off for everybody. Recorded on the audit trail.\","
         "\"requestBody\":{\"required\":true,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"exempt\":{\"type\":\"boolean\"},\"sensitivity\":{\"type\":\"string\",\"enum\":[\"default\",\"relaxed\",\"strict\"]}}}}}},"
         "\"responses\":{\"200\":{\"description\":\"The state as set\"},\"400\":{\"description\":\"A field the route does not take, or a wrong value\"},\"404\":{\"description\":\"No such account\"}}}}"
         ",\"/api/v1/accounts/{address}/review/unlock\":{\"post\":{\"summary\":\"Clear the compromise lock alone\","
         "\"description\":\"The lock the compromised-account detection put on the account is cleared and the account can sign in again; what was held for review stays held for its own review, and the failed-authentication lockout, if the account is behind that too, is not touched (POST /api/v1/accounts/{address}/unlock clears every lock at once). The unlock is a fresh start: what the account submitted before it does not count towards the next hour's volume or recipients, so the next message after a false positive is not the same shape again. unlocked says whether the account was locked. Recorded on the audit trail.\","
         "\"responses\":{\"200\":{\"description\":\"{unlocked}\"},\"404\":{\"description\":\"No such account\"}}}}";
      return AnsiString(paths);
   }
}
