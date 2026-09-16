// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See WebhookManager.h.

#include "StdAfx.h"

#include "WebhookManager.h"

#include "AlertManager.h"
#include "Crypt.h"
#include "HttpsClient.h"
#include "Time.h"
#include "Unicode.h"

#include "../BO/Domain.h"
#include "../Cache/CacheContainer.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../Util/Parsing/StringParser.h"

#include <ctime>
#include <set>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   const wchar_t *WebhookManager::EventMessageReceived = L"message.received";
   const wchar_t *WebhookManager::EventMessageDelivered = L"message.delivered";
   const wchar_t *WebhookManager::EventMessageBounced = L"message.bounced";
   const wchar_t *WebhookManager::EventMessageQuarantined = L"message.quarantined";
   const wchar_t *WebhookManager::EventAccountLocked = L"account.locked";

   namespace
   {
      // The subscriptions are re-read at most this often, so that a change made on
      // another node - or in another process of this one - is picked up without
      // every raise asking the database. A change made HERE invalidates at once.
      const __int64 CacheSeconds = 30;

      // What one body may carry. A subject is the only free text in it, and 255 is
      // what the quarantine and the archive already keep.
      const int MaxSubjectLength = 255;
      const int MaxStatusLength = 64;
      const int MaxReasonLength = 255;
      const int MaxErrorLength = 250;

      // Short, because this runs on a pass that a per-message stream wants back
      // quickly: an endpoint that has not answered in ten seconds has not answered,
      // and the delivery is retried rather than lost.
      const int RequestTimeoutSeconds = 10;
      const size_t MaxResponseBytes = 16 * 1024;

      // One pass attempts at most this many rows and runs for at most this long,
      // whichever comes first, so that a queue that has built up while a receiver
      // was down is worked off over several passes rather than holding the
      // scheduler thread - which is the backup's, the alert pass's and every other
      // task's thread too - for as long as it takes. The time cap is what bounds
      // the pass: the row cap alone would let two hundred rows for one endpoint
      // that has stopped answering cost two hundred deadlines.
      const int MaxPerPass = 200;
      const __int64 MaxPassSeconds = 60;

      // Over CHARACTERS, not bytes. The document is built wide and converted to
      // UTF-8 once, where it is sent; escaping the bytes of a narrowed copy would
      // mean the copy had already lost whatever the machine's code page lacks.
      String JsonEscape(const String &value)
      {
         String result;
         result.reserve(value.GetLength() + 8);

         for (int i = 0; i < value.GetLength(); i++)
         {
            wchar_t character = value[i];

            switch (character)
            {
            case '\"':
               result += _T("\\\"");
               break;
            case '\\':
               result += _T("\\\\");
               break;
            case '\r':
               result += _T("\\r");
               break;
            case '\n':
               result += _T("\\n");
               break;
            case '\t':
               result += _T("\\t");
               break;
            default:
               if (character >= 0x20)
                  result += character;
               break;
            }
         }

         return result;
      }

      String OneLine(const String &value)
      {
         String result = value;

         for (int i = 0; i < result.GetLength(); i++)
         {
            if (result[i] == '\r' || result[i] == '\n' || result[i] == '\t')
               result[i] = ' ';
         }

         return result;
      }

      String Shorten(const String &value, int maximum)
      {
         if (value.GetLength() <= maximum)
            return value;

         if (maximum <= 3)
            return value.Mid(0, maximum);

         return value.Mid(0, maximum - 3) + _T("...");
      }

      String TimeText(__int64 seconds)
      {
         time_t value = (time_t) seconds;

         tm utc = {};
         gmtime_s(&utc, &value);

         String text;
         text.Format(_T("%04d-%02d-%02d %02d:%02d:%02dZ"), utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
            utc.tm_hour, utc.tm_min, utc.tm_sec);

         return text;
      }
   }

   WebhookManager::WebhookManager() :
      cached_at_(0),
      cache_valid_(false)
   {

   }

   WebhookManager::~WebhookManager()
   {

   }

   bool
   WebhookManager::IsKnownEvent(const String &name)
   {
      for (const String &known : KnownEvents())
      {
         if (known == name)
            return true;
      }

      return false;
   }

   std::vector<String>
   WebhookManager::KnownEvents()
   {
      std::vector<String> events;

      events.push_back(EventMessageReceived);
      events.push_back(EventMessageDelivered);
      events.push_back(EventMessageBounced);
      events.push_back(EventMessageQuarantined);
      events.push_back(EventAccountLocked);

      return events;
   }

   /*
      A comma-separated list, compared without regard to case and with the spaces
      around each item ignored, because the list is typed by a person. An EMPTY
      list is "everything", which is what makes a subscription that names no
      domain the administrator saying "every domain" rather than "no domain".
   */
   bool
   WebhookManager::ListContains_(const String &list, const String &value)
   {
      if (list.IsEmpty())
         return true;

      if (value.IsEmpty())
         return false;

      std::vector<String> items = StringParser::SplitString(list, _T(","));

      for (String item : items)
      {
         item.Trim();

         if (!item.IsEmpty() && item.CompareNoCase(value.c_str()) == 0)
            return true;
      }

      return false;
   }

   bool
   WebhookManager::Matches_(const Subscription &subscription, const Event &event)
   {
      if (!subscription.enabled)
         return false;

      if (!ListContains_(subscription.events, event.name))
         return false;

      if (!ListContains_(subscription.domains, event.domain))
         return false;

      // An address list is matched against the local address the event is about
      // - the recipient of a delivery, the account that was locked - and not
      // against the sender, which is whoever wrote to us and is not ours to
      // subscribe to.
      if (!subscription.addresses.IsEmpty())
      {
         if (!ListContains_(subscription.addresses, event.account) &&
             !ListContains_(subscription.addresses, event.recipient))
            return false;
      }

      return true;
   }

   WebhookManager::Subscription
   WebhookManager::ReadSubscription_(const std::shared_ptr<DALRecordset> &recordset)
   {
      Subscription subscription;

      subscription.id = recordset->GetInt64Value("whid");
      subscription.name = recordset->GetStringValue("whname");
      subscription.url = recordset->GetStringValue("whurl");

      // Protected at rest the way a route's or a relaying domain's password is,
      // so that a copy of the database - a backup, a replica, a SQL login - does
      // not hand out what every body to this endpoint is signed with.
      subscription.secret = Crypt::Instance()->UnprotectSecret(recordset->GetStringValue("whsecret"));
      subscription.events = recordset->GetStringValue("whevents");
      subscription.domains = recordset->GetStringValue("whdomains");
      subscription.addresses = recordset->GetStringValue("whaddresses");
      subscription.enabled = recordset->GetLongValue("whenabled") != 0;
      subscription.max_per_minute = recordset->GetLongValue("whmaxperminute");
      subscription.include_subject = recordset->GetLongValue("whincludesubject") != 0;
      subscription.created = recordset->GetInt64Value("whcreated");

      return subscription;
   }

   WebhookManager::Delivery
   WebhookManager::ReadDelivery_(const std::shared_ptr<DALRecordset> &recordset)
   {
      Delivery delivery;

      delivery.id = recordset->GetInt64Value("wqid");
      delivery.subscription_id = recordset->GetInt64Value("wqwebhookid");
      delivery.event = recordset->GetStringValue("wqevent");
      delivery.created = recordset->GetInt64Value("wqcreated");
      delivery.state = recordset->GetLongValue("wqstate");
      delivery.tries = recordset->GetLongValue("wqtries");
      delivery.next = recordset->GetInt64Value("wqnext");
      delivery.error = recordset->GetStringValue("wqerror");
      delivery.body = recordset->GetStringValue("wqbody");

      return delivery;
   }

   bool
   WebhookManager::ReadSubscriptions_(std::vector<Subscription> &subscriptions)
   {
      subscriptions.clear();

      SQLCommand command(_T("select * from hm_webhooks order by whname asc"));

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         subscriptions.push_back(ReadSubscription_(recordset));
         recordset->MoveNext();
      }

      return true;
   }

   /*
      The subscriptions as the raise path sees them. This is the whole of "a
      server with no webhooks pays nothing": the common answer is a cached empty
      vector, and the database is asked at most twice a minute however much mail
      is flowing.
   */
   bool
   WebhookManager::CachedSubscriptions_(std::vector<Subscription> &subscriptions)
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      const __int64 now = (__int64) time(nullptr);

      // now < cached_at_ is a clock that went backwards, which must re-read
      // rather than freeze the cache until the clock catches up again.
      if (!cache_valid_ || now < cached_at_ || now - cached_at_ >= CacheSeconds)
      {
         std::vector<Subscription> fresh;

         if (!ReadSubscriptions_(fresh))
         {
            // The database is not answering. Whatever was last read stands, so
            // that a moment's unavailability does not silently stop a stream; a
            // first read that fails leaves the cache invalid and the next raise
            // tries again.
            if (!cache_valid_)
               return false;
         }
         else
         {
            cached_ = fresh;
            cached_at_ = now;
            cache_valid_ = true;
         }
      }

      subscriptions = cached_;
      return true;
   }

   void
   WebhookManager::Invalidate()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      cache_valid_ = false;
      cached_.clear();
      cached_at_ = 0;
   }

   /*
      The domain's own two switches. A domain that is not local - the recipient
      domain of a message this server relayed, say - has no row, and is treated as
      allowing the event with no subject: the event is about mail this server
      handled either way, and there is nobody to have made the decision.
   */
   bool
   WebhookManager::DomainAllows_(const String &domain, bool &subjectsAllowed)
   {
      subjectsAllowed = false;

      if (domain.IsEmpty())
         return true;

      std::shared_ptr<const Domain> stored = CacheContainer::Instance()->GetDomain(domain);

      if (!stored)
         return true;

      subjectsAllowed = stored->GetWebhookSubjects();
      return stored->GetWebhooksEnabled();
   }


   void
   WebhookManager::ScopeToLocalDomain(const String &sender, const String &recipient, String &domain, String &account)
   {
      domain = _T("");
      account = _T("");

      String recipientDomain = StringParser::ExtractDomain(recipient);
      recipientDomain.MakeLower();

      if (!recipientDomain.IsEmpty() && CacheContainer::Instance()->GetDomain(recipientDomain))
      {
         domain = recipientDomain;
         account = recipient;
         return;
      }

      String senderDomain = StringParser::ExtractDomain(sender);
      senderDomain.MakeLower();

      if (!senderDomain.IsEmpty() && CacheContainer::Instance()->GetDomain(senderDomain))
      {
         domain = senderDomain;
         account = sender;
      }
   }

   bool
   WebhookManager::Wants(const String &domain, const String &eventName)
   {
      if (!Configuration::Instance()->GetWebhooksEnabled())
         return false;

      std::vector<Subscription> subscriptions;
      if (!CachedSubscriptions_(subscriptions) || subscriptions.empty())
         return false;

      bool subjectsAllowed = false;
      if (!DomainAllows_(domain, subjectsAllowed))
         return false;

      for (const Subscription &subscription : subscriptions)
      {
         if (!subscription.enabled)
            continue;

         if (ListContains_(subscription.events, eventName) && ListContains_(subscription.domains, domain))
            return true;
      }

      return false;
   }

   bool
   WebhookManager::WantsSubject(const String &domain, const String &eventName)
   {
      if (!Configuration::Instance()->GetWebhooksEnabled())
         return false;

      // The domain decides first. Its switch being off means no event at all, and
      // its subject switch being off means no subject however many subscriptions
      // ask for one - which is what makes it an answer a domain administrator can
      // give once rather than a thing to police per subscription.
      bool subjectsAllowed = false;
      if (!DomainAllows_(domain, subjectsAllowed) || !subjectsAllowed)
         return false;

      std::vector<Subscription> subscriptions;
      if (!CachedSubscriptions_(subscriptions))
         return false;

      for (const Subscription &subscription : subscriptions)
      {
         if (!subscription.enabled || !subscription.include_subject)
            continue;

         if (ListContains_(subscription.events, eventName) && ListContains_(subscription.domains, domain))
            return true;
      }

      return false;
   }

   /*
      The ceiling, per subscription and per minute. Deliberately in memory: it is a
      question about a RATE, and a rate is not configuration - it would cost a
      count query per event to keep in the database, on the very path that must
      stay cheap. What it costs is that a restart forgives whatever the minute had
      used, which is one minute's worth on a server that has just started.
   */
   bool
   WebhookManager::WithinCeiling_(const Subscription &subscription)
   {
      const int configured = subscription.max_per_minute > 0
         ? subscription.max_per_minute
         : Configuration::Instance()->GetWebhookMaxPerMinute();

      const __int64 now = (__int64) time(nullptr);
      const __int64 minute = now / 60;

      bool within = true;
      bool sayIt = false;
      int dropped = 0;

      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);

         Rate &rate = rates_[subscription.name];

         if (rate.minute != minute)
         {
            rate.minute = minute;
            rate.count = 0;
         }

         if (rate.count >= configured)
         {
            rate.dropped++;
            dropped = rate.dropped;
            within = false;

            // Once a minute per subscription, not once per event: the line exists
            // to tell an administrator that a ceiling is being hit, and a line per
            // dropped event would be the flood this is here to prevent.
            if (rate.said != minute)
            {
               rate.said = minute;
               sayIt = true;
            }
         }
         else
         {
            rate.count++;
         }
      }

      // With the lock RELEASED: LOG_APPLICATION takes the process-wide logger
      // mutex and writes a file, and this lock is taken on the delivery path by
      // every thread that raises an event. AccountLockout separates the two for
      // the same reason.
      if (sayIt)
      {
         LOG_APPLICATION(Formatter::Format("Webhooks: subscription '{0}' is at its ceiling of {1} event(s) a minute; events for it are being dropped. {2} have been dropped since the server started. Raise its rate limit, or narrow what it subscribes to.",
            subscription.name, configured, dropped));
      }

      return within;
   }

   int
   WebhookManager::DroppedSinceStart(const String &name)
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);

      std::map<String, Rate>::const_iterator found = rates_.find(name);

      return found == rates_.end() ? 0 : found->second.dropped;
   }

   String
   WebhookManager::BuildEventJson(const Subscription &subscription, const Event &event, __int64 when)
   {
      String json;
      json += _T("{");
      json += _T("\"event\":\"") + JsonEscape(event.name) + _T("\",");
      json += _T("\"subscription\":\"") + JsonEscape(subscription.name) + _T("\",");

      String numbers;
      numbers.Format(_T("\"time\":%I64d,"), when);
      json += numbers;

      json += _T("\"time_utc\":\"") + JsonEscape(TimeText(when)) + _T("\",");
      json += _T("\"server\":\"") + JsonEscape(Configuration::Instance()->GetHostName()) + _T("\"");

      // From here on, only what the event actually carries. A member that is
      // absent is a thing this event does not have, so a receiver never has to
      // decide whether an empty string means "none" or "not told".
      if (!event.domain.IsEmpty())
         json += _T(",\"domain\":\"") + JsonEscape(event.domain) + _T("\"");

      if (!event.account.IsEmpty())
         json += _T(",\"account\":\"") + JsonEscape(event.account) + _T("\"");

      if (!event.sender.IsEmpty())
         json += _T(",\"sender\":\"") + JsonEscape(event.sender) + _T("\"");

      if (!event.recipient.IsEmpty())
         json += _T(",\"recipient\":\"") + JsonEscape(event.recipient) + _T("\"");

      if (event.queue_id > 0)
      {
         String queueId;
         queueId.Format(_T(",\"queue_id\":%I64d"), event.queue_id);
         json += queueId;
      }

      if (event.size > 0)
      {
         String size;
         size.Format(_T(",\"size\":%I64d"), event.size);
         json += size;
      }

      // The one piece of content a body can hold, and only where the domain and
      // the subscription both said so. The caller has already read the domain's
      // answer; this is the subscription's.
      if (subscription.include_subject && !event.subject.IsEmpty())
         json += _T(",\"subject\":\"") + JsonEscape(Shorten(OneLine(event.subject), MaxSubjectLength)) + _T("\"");

      if (!event.status.IsEmpty())
         json += _T(",\"status\":\"") + JsonEscape(Shorten(OneLine(event.status), MaxStatusLength)) + _T("\"");

      if (!event.reason.IsEmpty())
         json += _T(",\"reason\":\"") + JsonEscape(Shorten(OneLine(event.reason), MaxReasonLength)) + _T("\"");

      if (event.has_number)
      {
         String number;
         number.Format(_T(",\"number\":%I64d"), event.number);
         json += number;
      }

      json += _T("}");

      return json;
   }

   bool
   WebhookManager::Enqueue_(const Subscription &subscription, const Event &event, __int64 now)
   {
      SQLStatement statement(SQLStatement::STInsert, _T("hm_webhookqueue"));
      statement.AddColumnInt64(_T("wqwebhookid"), subscription.id);
      statement.AddColumn(_T("wqevent"), event.name, 64);
      statement.AddColumnInt64(_T("wqcreated"), now);
      statement.AddColumn(_T("wqstate"), (long) QueuePending);
      statement.AddColumn(_T("wqtries"), (long) 0);

      // Due at once. The first attempt is made by the next pass rather than from
      // here, which is the deferral the whole design turns on.
      statement.AddColumnInt64(_T("wqnext"), now);
      statement.AddColumn(_T("wqerror"), _T(""), 255);
      statement.AddColumn(_T("wqbody"), BuildEventJson(subscription, event, now));
      statement.SetIdentityColumn(_T("wqid"));

      String errorMessage;
      if (!Application::Instance()->GetDBManager()->Execute(statement.GetCommand(), nullptr, 0, errorMessage))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6970, "WebhookManager::Enqueue_",
            "A webhook event could not be recorded and will therefore not be delivered: " + errorMessage);
         return false;
      }

      return true;
   }

   int
   WebhookManager::Raise(const Event &event)
   {
      if (event.name.IsEmpty())
         return 0;

      if (!Configuration::Instance()->GetWebhooksEnabled())
         return 0;

      std::vector<Subscription> subscriptions;

      // The fast path, and the one nearly every installation takes: no
      // subscription, no database work, no allocation beyond an empty vector.
      if (!CachedSubscriptions_(subscriptions) || subscriptions.empty())
         return 0;

      bool subjectsAllowed = false;
      if (!DomainAllows_(event.domain, subjectsAllowed))
         return 0;

      Event carried = event;

      // The domain's answer is applied HERE rather than at each subscription, so
      // that a body cannot carry a subject because one subscription asked when
      // the domain said no.
      if (!subjectsAllowed)
         carried.subject = _T("");

      int queued = 0;

      for (const Subscription &subscription : subscriptions)
      {
         if (!Matches_(subscription, carried))
            continue;

         if (!WithinCeiling_(subscription))
            continue;

         if (Enqueue_(subscription, carried, (__int64) time(nullptr)))
            queued++;
      }

      return queued;
   }

   bool
   WebhookManager::GetSubscriptions(std::vector<Subscription> &subscriptions)
   {
      return ReadSubscriptions_(subscriptions);
   }

   bool
   WebhookManager::GetSubscription(const String &name, Subscription &subscription)
   {
      // whname is nvarchar(64), and the name comes from a REST path.
      if (name.IsEmpty() || name.GetLength() > 64)
         return false;

      // Names are stored lower case, and the comparison is made here rather than
      // by each caller: PostgreSQL compares = case-sensitively and the other three
      // do not, so a path spelled /api/v1/webhooks/Ticketing would find the row on
      // three backends and answer 404 on the fourth.
      String wanted = name;
      wanted.MakeLower();

      SQLCommand command(_T("select * from hm_webhooks where whname = @NAME"));
      command.AddParameter("@NAME", wanted);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      subscription = ReadSubscription_(recordset);
      return true;
   }

   bool
   WebhookManager::SaveSubscription(const Subscription &subscription, bool creating, String &refusal)
   {
      refusal = _T("");

      String name = subscription.name;
      name.Trim();
      name.MakeLower();

      // Lower case on every backend, for the reason AlertManager::SaveRule gives:
      // PostgreSQL compares = case-sensitively, so "Billing" and "billing" would be
      // two subscriptions there and one everywhere else.
      if (name.IsEmpty() || name.GetLength() > 64)
      {
         refusal = _T("a subscription's name is between one and sixty-four characters");
         return false;
      }

      for (int i = 0; i < name.GetLength(); i++)
      {
         const wchar_t character = name[i];

         // The name is the identifier in a REST path and in hmctl's arguments, so
         // it is held to what can be one without quoting or escaping.
         if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '-' || character == '_' || character == '.'))
         {
            refusal = _T("a subscription's name holds letters, digits, and - _ . only");
            return false;
         }
      }

      String url = subscription.url;
      url.Trim();

      if (url.IsEmpty())
      {
         refusal = _T("a subscription needs the address of an endpoint to call");
         return false;
      }

      if (url.GetLength() > 1024)
      {
         refusal = _T("an endpoint address is at most 1024 characters");
         return false;
      }

      {
         bool https = false;
         AnsiString host, port, path;

         if (!HttpsClient::ParseUrl(AnsiString(url), https, host, port, path))
         {
            refusal = _T("an endpoint address is an http or https URL");
            return false;
         }

         // Plain http is accepted only to this machine - the same rule HttpsClient
         // itself applies, said here so that it is a refusal at the point of
         // configuration rather than a delivery that fails for ever afterwards.
         if (!https && !HttpsClient::IsLoopbackHost(host))
         {
            refusal = _T("an endpoint reached over plain http must be on this machine; anywhere else needs https, since the body says who wrote to whom");
            return false;
         }
      }

      // A signature nobody can check is not a signature, and a body arriving at a
      // URL somebody found in a configuration file is an event anyone can forge.
      if (subscription.secret.IsEmpty())
      {
         refusal = _T("a subscription needs a shared secret, so the bodies it receives can be verified");
         return false;
      }

      if (subscription.secret.GetLength() > 255)
      {
         refusal = _T("a shared secret is at most 255 characters");
         return false;
      }

      // The envelope ProtectSecret writes is longer than what it protects, which
      // is why whsecret is 1024 wide for a secret of at most 255. On Linux a
      // protection that fails answers with an empty string rather than the clear
      // text; a subscription whose secret cannot be kept is refused rather than
      // stored with nothing to sign under.
      const String protectedSecret = Crypt::Instance()->ProtectSecret(subscription.secret);

      if (protectedSecret.IsEmpty())
      {
         refusal = _T("the shared secret could not be protected for storage; see the error log");
         return false;
      }

      {
         std::vector<String> requested = StringParser::SplitString(subscription.events, _T(","));

         for (String requestedEvent : requested)
         {
            requestedEvent.Trim();

            if (!requestedEvent.IsEmpty() && !IsKnownEvent(requestedEvent))
            {
               refusal = _T("this server raises message.received, message.delivered, message.bounced, message.quarantined and account.locked; it does not raise ") + requestedEvent;
               return false;
            }
         }
      }

      if (subscription.events.GetLength() > 255 || subscription.domains.GetLength() > 1024 ||
          subscription.addresses.GetLength() > 1024)
      {
         refusal = _T("a subscription's event list is at most 255 characters and its domain and address lists 1024 each");
         return false;
      }

      if (subscription.max_per_minute < 0 || subscription.max_per_minute > 100000)
      {
         refusal = _T("a rate limit is between 0 (the server's WebhookMaxPerMinute) and 100000 events a minute");
         return false;
      }

      Subscription existing;
      const bool exists = GetSubscription(name, existing);

      if (creating && exists)
      {
         refusal = _T("a subscription called ") + name + _T(" exists already");
         return false;
      }

      if (!creating && !exists)
      {
         refusal = _T("there is no subscription called ") + name;
         return false;
      }

      SQLStatement statement(exists ? SQLStatement::STUpdate : SQLStatement::STInsert, _T("hm_webhooks"));

      if (!exists)
      {
         statement.AddColumn(_T("whname"), name, 64);
         statement.AddColumnInt64(_T("whcreated"), (__int64) time(nullptr));
      }

      statement.AddColumn(_T("whurl"), url, 1024);
      statement.AddColumn(_T("whsecret"), protectedSecret, 1024);
      statement.AddColumn(_T("whevents"), subscription.events, 255);
      statement.AddColumn(_T("whdomains"), subscription.domains, 1024);
      statement.AddColumn(_T("whaddresses"), subscription.addresses, 1024);
      statement.AddColumn(_T("whenabled"), (long) (subscription.enabled ? 1 : 0));
      statement.AddColumn(_T("whmaxperminute"), (long) subscription.max_per_minute);
      statement.AddColumn(_T("whincludesubject"), (long) (subscription.include_subject ? 1 : 0));

      if (exists)
         statement.AddWhereClauseColumn(_T("whname"), name);
      else
         statement.SetIdentityColumn(_T("whid"));

      String errorMessage;
      if (!Application::Instance()->GetDBManager()->Execute(statement.GetCommand(), nullptr, 0, errorMessage))
      {
         refusal = _T("the subscription could not be saved: ") + errorMessage;
         return false;
      }

      // The minute's count is kept by name, so a subscription created under a
      // name that was deleted a moment ago must not inherit what the old one
      // had already spent of this minute.
      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);
         rates_.erase(name);
      }

      Invalidate();
      return true;
   }

   bool
   WebhookManager::DeleteSubscription(const String &name)
   {
      Subscription existing;
      if (!GetSubscription(name, existing))
         return false;

      // The queue rows go with it, through the foreign key: a delivery to an
      // endpoint that no longer exists is a request nobody wants made.
      SQLCommand command(_T("delete from hm_webhooks where whname = @NAME"));
      command.AddParameter("@NAME", existing.name);

      if (!Application::Instance()->GetDBManager()->Execute(command))
         return false;

      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);
         rates_.erase(existing.name);
      }

      Invalidate();
      return true;
   }

   bool
   WebhookManager::GetDeliveries(const String &name, int state, int limit, std::vector<Delivery> &deliveries)
   {
      deliveries.clear();

      if (limit < 1)
         limit = 1;
      if (limit > 1000)
         limit = 1000;

      std::vector<Subscription> subscriptions;
      if (!ReadSubscriptions_(subscriptions))
         return false;

      __int64 subscriptionId = 0;

      if (!name.IsEmpty())
      {
         Subscription one;
         if (!GetSubscription(name, one))
            return true;   // a name nobody uses has no deliveries, which is not an error

         subscriptionId = one.id;
      }

      String sql = _T("select * from hm_webhookqueue");
      String where;

      if (subscriptionId > 0)
         where = _T("wqwebhookid = @WEBHOOKID");

      if (state > 0)
         where = where.IsEmpty() ? String(_T("wqstate = @STATE")) : where + _T(" and wqstate = @STATE");

      if (!where.IsEmpty())
         sql += _T(" where ") + where;

      sql += _T(" order by wqid desc");

      SQLCommand command(sql);

      if (subscriptionId > 0)
         command.AddParameter("@WEBHOOKID", subscriptionId);
      if (state > 0)
         command.AddParameter("@STATE", state);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF() && (int) deliveries.size() < limit)
      {
         Delivery delivery = ReadDelivery_(recordset);

         for (const Subscription &subscription : subscriptions)
         {
            if (subscription.id == delivery.subscription_id)
            {
               delivery.subscription = subscription.name;
               break;
            }
         }

         deliveries.push_back(delivery);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   WebhookManager::CountDeliveries(__int64 subscriptionId, int &pending, int &delivered, int &deadLettered)
   {
      pending = 0;
      delivered = 0;
      deadLettered = 0;

      // One grouped read rather than three counts: this is asked once per
      // subscription by the listing, and a server with a dozen subscriptions
      // would otherwise make three dozen round trips to draw one page.
      SQLCommand command(_T("select wqstate, count(*) as statecount from hm_webhookqueue where wqwebhookid = @WEBHOOKID group by wqstate"));
      command.AddParameter("@WEBHOOKID", subscriptionId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         const int state = recordset->GetLongValue("wqstate");
         const int count = recordset->GetLongValue("statecount");

         switch (state)
         {
         case QueuePending:      pending = count; break;
         case QueueDelivered:    delivered = count; break;
         case QueueDeadLettered: deadLettered = count; break;
         default: break;
         }

         recordset->MoveNext();
      }

      return true;
   }

   bool
   WebhookManager::UpdateDelivery_(__int64 id, int state, int tries, __int64 next, const String &error)
   {
      SQLCommand command(_T("update hm_webhookqueue set wqstate = @STATE, wqtries = @TRIES, wqnext = @NEXT, wqerror = @ERROR where wqid = @ID"));
      command.AddParameter("@STATE", state);
      command.AddParameter("@TRIES", tries);
      command.AddParameter("@NEXT", next);
      command.AddParameter("@ERROR", Shorten(OneLine(error), MaxErrorLength));
      command.AddParameter("@ID", id);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   WebhookManager::Deliver_(const Subscription &subscription, const Delivery &delivery, String &error, bool &unreachable)
   {
      unreachable = false;

      const __int64 timestamp = (__int64) time(nullptr);

      // The body was built and stored when the event was raised, and is sent
      // BYTE FOR BYTE as it was stored: the signature is over those bytes, so a
      // retry that rebuilt the body would produce a second document with a second
      // signature and a receiver de-duplicating on the delivery id would see two
      // different messages under one identity.
      // UTF-8, and the same bytes are signed and sent. A receiver verifies what it
      // received, so the signature has to be over the bytes that go out - not over a
      // second narrowing of the same characters through the machine's code page.
      AnsiString body;
      Unicode::WideToMultiByte(delivery.body, body);

      AnsiString secret;
      Unicode::WideToMultiByte(subscription.secret, secret);

      const AnsiString signature = AlertManager::SignWebhook(secret, timestamp, body);

      std::vector<AnsiString> headers;

      AnsiString timestampHeader;
      timestampHeader.Format("X-hMailServer-Timestamp: %I64d", timestamp);
      headers.push_back(timestampHeader);
      headers.push_back("X-hMailServer-Signature: sha256=" + signature);
      AnsiString eventName;
      Unicode::WideToMultiByte(delivery.event, eventName);
      headers.push_back("X-hMailServer-Event: " + eventName);

      // The queue row's id, so that a receiver can tell a retry from a second
      // event, and which attempt this is, so that it can tell how long this one
      // has been trying.
      AnsiString deliveryHeader;
      deliveryHeader.Format("X-hMailServer-Delivery: %I64d", delivery.id);
      headers.push_back(deliveryHeader);

      AnsiString attemptHeader;
      attemptHeader.Format("X-hMailServer-Attempt: %d", delivery.tries + 1);
      headers.push_back(attemptHeader);

      headers.push_back("User-Agent: hMailServer");

      HttpsClient::Response response;

      AnsiString url;
      Unicode::WideToMultiByte(subscription.url, url);

      // Request, not RequestPublic: an endpoint may be an address only this server
      // can reach - a receiver on the same network is the common case - and the
      // policy is stated in Webhooks.md and enforced where a subscription is made:
      // only the administrator password or a role that reaches every domain and
      // holds manage_server can name one, and no API key can.
      if (!HttpsClient::Request("POST", url, headers, "application/json", body,
                                response, error, RequestTimeoutSeconds, MaxResponseBytes))
      {
         unreachable = true;
         return false;
      }

      if (response.status_code < 200 || response.status_code > 299)
      {
         error.Format(_T("the endpoint answered %d"), response.status_code);
         return false;
      }

      return true;
   }

   int
   WebhookManager::RunDeliveries()
   {
      if (!Configuration::Instance()->GetWebhooksEnabled())
         return 0;

      // One pass at a time; see the header. A REST worker that asked while the
      // scheduled pass is running waits for it and then finds what it left, which
      // is nothing if the first pass finished the queue.
      boost::lock_guard<boost::mutex> pass(pass_mutex_);

      const __int64 now = (__int64) time(nullptr);

      SQLCommand command(_T("select * from hm_webhookqueue where wqstate = 1 and wqnext <= @NOW order by wqid asc"));
      command.AddParameter("@NOW", now);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return 0;

      std::vector<Delivery> due;
      while (!recordset->IsEOF() && (int) due.size() < MaxPerPass)
      {
         due.push_back(ReadDelivery_(recordset));
         recordset->MoveNext();
      }

      if (due.empty())
         return 0;

      std::vector<Subscription> subscriptions;
      if (!ReadSubscriptions_(subscriptions))
         return 0;

      const int maximumAttempts = Configuration::Instance()->GetWebhookMaxAttempts();
      int delivered = 0;

      // Subscriptions whose endpoint gave no answer at all in this pass. Their
      // remaining rows are left as they are - still due, still pending, attempts
      // unchanged - for the next pass, so that an endpoint that is down costs one
      // deadline a pass and not one per row. A 4xx or 5xx is an answer, and does
      // not put a subscription here: the endpoint is up, and the next row may be
      // one it accepts.
      std::set<__int64> unreachableSubscriptions;
      int heldForNextPass = 0;

      for (const Delivery &delivery : due)
      {
         if ((__int64) time(nullptr) - now >= MaxPassSeconds)
         {
            // Out of time. What is left stays due and the next pass takes it.
            heldForNextPass++;
            continue;
         }

         if (unreachableSubscriptions.find(delivery.subscription_id) != unreachableSubscriptions.end())
         {
            heldForNextPass++;
            continue;
         }

         const Subscription *subscription = nullptr;

         for (const Subscription &candidate : subscriptions)
         {
            if (candidate.id == delivery.subscription_id)
            {
               subscription = &candidate;
               break;
            }
         }

         if (subscription == nullptr)
         {
            // Cannot normally happen - the foreign key removes the queue with the
            // subscription - but a row with nowhere to go must not be retried for
            // ever if it ever does.
            UpdateDelivery_(delivery.id, QueueDeadLettered, delivery.tries, 0,
               _T("the subscription this was queued for no longer exists"));
            continue;
         }

         if (!subscription->enabled)
         {
            // Switched off between the event and now. Held rather than dropped:
            // switching a subscription off is not the same as saying its receiver
            // should never hear what it missed, and a pass after it is switched
            // on again will make the attempt.
            continue;
         }

         String error;
         bool unreachable = false;

         if (Deliver_(*subscription, delivery, error, unreachable))
         {
            UpdateDelivery_(delivery.id, QueueDelivered, delivery.tries + 1, 0, _T(""));
            delivered++;
            continue;
         }

         if (unreachable)
            unreachableSubscriptions.insert(delivery.subscription_id);

         const int tries = delivery.tries + 1;

         if (tries >= maximumAttempts)
         {
            // The dead letter. The row stays, with the body and the reason, so the
            // event is not lost - it simply stops being retried, and the
            // administrator is told once that an endpoint of theirs is not
            // answering.
            UpdateDelivery_(delivery.id, QueueDeadLettered, tries, 0, error);

            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6971, "WebhookManager::RunDeliveries",
               Formatter::Format("Webhooks: event {0} for subscription '{1}' was not delivered after {2} attempt(s) and has been dead-lettered. The last failure was: {3}. The body is still recorded and readable at /api/v1/webhooks/deliveries.",
                  delivery.event, subscription->name, tries, error));
            continue;
         }

         // AlertManager's ladder, for the same reasons and keyed the same way: on
         // the attempts made BEFORE this one, so none before means no wait and the
         // first retry is on the next pass. 0, 1, 5, 15, 60, 60 ... minutes.
         UpdateDelivery_(delivery.id, QueuePending, tries,
            now + (__int64) AlertManager::WebhookBackoffMinutes(tries - 1) * 60, error);
      }

      if (heldForNextPass > 0)
      {
         LOG_APPLICATION(Formatter::Format("Webhooks: {0} delivery(ies) were left for the next pass - {1} subscription(s) gave no answer at all, or the pass reached its {2} second limit. Nothing has been lost; they are still due.",
            heldForNextPass, (int) unreachableSubscriptions.size(), (int) MaxPassSeconds));
      }

      return delivered;
   }

   /*
      Retention. A per-message stream fills a table, and a delivered row has done
      its job: what it is kept for is the administrator asking "did that reach
      them", which is a question about the last few days. A dead letter is kept
      for exactly as long, because it is the same question with a worse answer.
      0 days is "keep everything", which is a decision somebody has to make rather
      than the default.
   */
   int
   WebhookManager::PurgeOldDeliveries()
   {
      const int days = Configuration::Instance()->GetWebhookRetentionDays();

      if (days <= 0)
         return 0;

      const __int64 cutoff = (__int64) time(nullptr) - (__int64) days * 86400;

      SQLCommand count(_T("select count(*) as oldcount from hm_webhookqueue where wqstate <> 1 and wqcreated < @CUTOFF"));
      count.AddParameter("@CUTOFF", cutoff);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(count);
      if (!recordset || recordset->IsEOF())
         return 0;

      const int removable = recordset->GetLongValue("oldcount");
      if (removable <= 0)
         return 0;

      SQLCommand command(_T("delete from hm_webhookqueue where wqstate <> 1 and wqcreated < @CUTOFF"));
      command.AddParameter("@CUTOFF", cutoff);

      if (!Application::Instance()->GetDBManager()->Execute(command))
         return 0;

      return removable;
   }

   WebhookTask::WebhookTask()
   {

   }

   WebhookTask::~WebhookTask()
   {

   }

   void
   WebhookTask::DoWork()
   {
      WebhookManager::Instance()->RunDeliveries();
      WebhookManager::Instance()->PurgeOldDeliveries();
   }
}
