// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "ComplaintTracker.h"

#include "BounceTracker.h"
#include "DeliveryReportParser.h"
#include "FeedbackReportParser.h"

#include "../Common/BO/Account.h"
#include "../Common/BO/Domain.h"
#include "../Common/BO/Message.h"
#include "../Common/BO/MessageRecipient.h"
#include "../Common/BO/MessageRecipients.h"
#include "../Common/Cache/CacheContainer.h"
#include "../Common/Persistence/PersistentBounce.h"
#include "../Common/Persistence/PersistentComplaint.h"
#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Persistence/MessageStore.h"
#include "../Common/Util/MessageOrigin.h"
#include "../Common/Util/Unicode.h"
#include "../Common/Util/Hashing/HashCreator.h"

#include <algorithm>
#include <ctime>
#include <set>
#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The rates, read from the database at most this often. The metrics endpoint
      // and the alert pass share them: a Prometheus scrape every fifteen seconds and
      // an alert pass every minute must not each be a query.
      const time_t RateCacheSeconds = 60;

      boost::mutex rate_mutex_;
      std::vector<ComplaintTracker::Rate> rate_cache_;
      time_t rate_cache_read_at_ = 0;
      bool rate_cache_valid_ = false;

      // Bumped every time a complaint is recorded or the sweep removes rows. A
      // reader takes a copy of it before it queries - without the lock held, which
      // is the point - and publishes what it read only if the number is still the
      // same. Otherwise a complaint written while a read was in flight would be
      // undone by that read's older snapshot being marked fresh for a minute.
      unsigned long rate_generation_ = 0;

      // A tally that cannot be written is reported at most this often. Once per
      // message would fill the error log from the delivery path, and an ERROR entry
      // is not a thing to write once per message on a server that is already having
      // trouble with its database.
      boost::mutex tally_mutex_;
      time_t tally_reported_at_ = 0;
      const time_t TallyReportIntervalSeconds = 3600;

      AnsiString Utf8(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);
         return utf8;
      }

      // A JSON string body, from wide text, as UTF-8.
      AnsiString JsonText(const String &value)
      {
         const AnsiString utf8 = Utf8(value);
         AnsiString result;

         for (int i = 0; i < utf8.GetLength(); i++)
         {
            const char c = utf8.GetAt(i);

            switch (c)
            {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
               if ((unsigned char) c < 0x20)
               {
                  char escaped[8];
                  snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned char) c);
                  result += escaped;
               }
               else
                  result += c;
               break;
            }
         }

         return result;
      }

      AnsiString JsonField(const char *name, const String &value)
      {
         return AnsiString("\"") + name + "\":\"" + JsonText(value) + "\"";
      }

      AnsiString JsonNumber(const char *name, __int64 value)
      {
         AnsiString text;
         text.Format("\"%s\":%I64d", name, value);
         return text;
      }

      AnsiString JsonBool(const char *name, bool value)
      {
         return AnsiString("\"") + name + "\":" + (value ? "true" : "false");
      }

      String DomainName(__int64 domainID)
      {
         std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(domainID);
         return domain ? domain->GetName() : String();
      }

      void Expect(bool condition, const char *message)
      {
         if (!condition)
            throw std::logic_error(message);
      }
   }

   bool
   ComplaintTracker::Enabled_()
   {
      Configuration *configuration = Configuration::Instance();
      return configuration && configuration->GetSettings() && configuration->GetComplaintProcessingEnabled();
   }

   String
   ComplaintTracker::Mailbox_()
   {
      Configuration *configuration = Configuration::Instance();
      return (configuration && configuration->GetSettings()) ? configuration->GetComplaintMailbox() : String();
   }

   bool
   ComplaintTracker::IsComplaintMailbox(const String &configured, const String &address)
   {
      const String wanted = PersistentBounce::Canonical(address);
      if (wanted.IsEmpty())
         return false;

      String list = configured;
      list.Trim();
      if (list.IsEmpty())
         return false;

      // Separated by commas, semicolons or white space, so a list typed any of the
      // ways an administrator types one reads the same.
      list.Replace(_T(";"), _T(","));
      list.Replace(_T(" "), _T(","));
      list.Replace(_T("\t"), _T(","));

      std::vector<String> names = StringParser::SplitString(list, _T(","));

      for (const String &name : names)
      {
         if (PersistentBounce::Canonical(name) == wanted)
            return true;
      }

      return false;
   }

   bool
   ComplaintTracker::CountsAsComplaint(const String &feedbackType)
   {
      String type = feedbackType;
      type.Trim();
      type.ToLower();

      if (type.IsEmpty())
         return false;

      // RFC 5965 3.1 and the IANA registry: not-spam is the reader saying a message
      // was wrongly filtered, which is the opposite of a complaint. It is recorded,
      // because knowing it happened is worth something, and it counts towards
      // nothing. Everything else - abuse, fraud, virus, and a type a provider
      // invents tomorrow - is somebody saying they did not want this mail.
      return type != _T("not-spam");
   }

   String
   ComplaintTracker::ComplaintKey(const String &messageId, const String &address, const String &sender,
                                  const String &arrival, __int64 fallback)
   {
      // The Message-ID as the report's original headers carry it, or as a
      // caller hands it over: with or without its angle brackets, in any case.
      // The same report read twice - or by two nodes - has to make one key.
      String identity = DeliveryReportParser::NormaliseMessageId(messageId);
      identity.Trim();
      identity.ToLower();

      String when = arrival;
      when.Trim();

      // Nothing in the report identifies the message complained about. Two genuine
      // complaints about two different messages would then share a key and the
      // second would be dropped as a repeat, so the local message's own id goes in
      // instead: every report is then its own complaint, and a provider that sends
      // the same one twice is counted twice. Stated in docs/Complaints.md.
      if (identity.IsEmpty() && when.IsEmpty())
         identity = Formatter::Format(_T("local:{0}"), fallback);

      const String material = identity + _T("\n") +
                              PersistentBounce::Canonical(address) + _T("\n") +
                              PersistentBounce::Canonical(sender) + _T("\n") +
                              when;

      HashCreator hasher(HashCreator::SHA256);
      return String(hasher.GenerateHashNoSalt(Utf8(material), HashCreator::hex));
   }

   __int64
   ComplaintTracker::AccountIDFor_(const String &address)
   {
      const String canonical = PersistentBounce::Canonical(address);
      if (canonical.IsEmpty())
         return 0;

      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(canonical);
      return account ? account->GetID() : 0;
   }

   void
   ComplaintTracker::InvalidateRates_()
   {
      boost::lock_guard<boost::mutex> guard(rate_mutex_);
      rate_cache_valid_ = false;
      rate_generation_++;
   }

   void
   ComplaintTracker::ReportTallyUnwritable_(const String &detail)
   {
      {
         boost::lock_guard<boost::mutex> guard(tally_mutex_);

         const time_t now = time(nullptr);

         if (tally_reported_at_ != 0 && tally_reported_at_ <= now &&
             now - tally_reported_at_ < TallyReportIntervalSeconds)
            return;

         tally_reported_at_ = now;
      }

      ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6933, "ComplaintTracker",
         "The complaint rate's tally of messages sent could not be written, so the complaint rate will read higher than it is. " +
         AnsiString(Utf8(detail)) + " This is reported at most once an hour while it lasts.");
   }

   void
   ComplaintTracker::InspectIncomingComplaint(std::shared_ptr<Message> message)
   {
      if (!message || !Enabled_())
         return;

      const String mailbox = Mailbox_();
      if (mailbox.IsEmpty())
         return;

      // The local recipients, by the address the report was addressed to. An alias
      // that expands to several accounts is still one address here.
      std::vector<String> localAddresses;
      std::set<String> distinct;

      // owned by this call: the recipients of the message this delivery holds
      for (std::shared_ptr<MessageRecipient> recipient : message->GetRecipients()->GetVector())
      {
         if (recipient->GetLocalAccountID() <= 0)
            continue;

         const String original = recipient->GetOriginalAddress().IsEmpty() ? recipient->GetAddress() : recipient->GetOriginalAddress();
         if (distinct.insert(PersistentBounce::Canonical(original)).second)
            localAddresses.push_back(original);
      }

      // Addressed to the complaint mailbox and to nothing else. A provider posts a
      // report to the one address it was registered against; a message carrying a
      // report to that address AND to somebody else is somebody making this server
      // read a report on the way past.
      if (localAddresses.size() != 1 || !IsComplaintMailbox(mailbox, localAddresses[0]))
         return;

      const String fileName = MessageStore::WorkingFile(message);

      FeedbackReport report;
      if (!FeedbackReportParser::ParseFile(fileName, report))
      {
         // The message is delivered to the mailbox unchanged, as everything here is:
         // a report this server cannot read is still mail somebody sent, and the
         // administrator reading that mailbox is the one who can say what it was.
         LOG_APPLICATION(Formatter::Format(_T("Complaints: message {0} to the complaint mailbox {1} is not an RFC 5965 feedback report this server can read; it is delivered unchanged and nothing is recorded."),
            message->GetID(), localAddresses[0]));
         return;
      }

      // Whose mail was complained about. A chain of three, tried in order, and each
      // step is taken when the one before it does not RESOLVE - not merely when it
      // is absent. That distinction is the whole point: Original-Mail-From is the
      // envelope sender the provider saw, and on an installation with BATV or SRS
      // on - both of which this server does, in SMTPClientConnection - that is a
      // prvs= or SRS0= address, which SenderDomainID answers 0 for by design (a
      // return path is not a sender, and an SRS forward's mail belongs to whoever
      // wrote it). Stopping there would drop every report about that mail unless
      // the provider happened to send Reported-Domain, although the returned
      // message's own From names the domain outright.
      //
      // Whichever step resolves is also what is recorded as the sender and looked
      // up as the account, so the row names the real sender rather than the return
      // path that wrapped it.
      String sender = report.original_mail_from;
      __int64 domainID = BounceTracker::SenderDomainID(sender);

      if (domainID <= 0 && !report.original_from.IsEmpty())
      {
         const __int64 fromHeader = BounceTracker::SenderDomainID(report.original_from);

         if (fromHeader > 0)
         {
            sender = report.original_from;
            domainID = fromHeader;
         }
      }

      if (sender.IsEmpty())
         sender = report.original_from;

      if (domainID <= 0 && !report.reported_domain.IsEmpty())
         domainID = BounceTracker::DomainIDByName(report.reported_domain);

      if (domainID <= 0)
      {
         // A report about mail this server did not send is not this server's to
         // record: no row is written, so no report from outside can create one.
         LOG_APPLICATION(Formatter::Format(_T("Complaints: the feedback report in message {0} is about {1}, which is not a domain this server hosts; it is delivered unchanged and nothing is recorded."),
            message->GetID(), report.reported_domain.IsEmpty() ? sender : report.reported_domain));
         return;
      }

      // Who complained. Original-Rcpt-To, then the returned message's To. A
      // provider that redacts both leaves the complaint with no address, which is
      // recorded and counted towards the domain's rate and lists nobody.
      String complained = report.original_rcpt_to.IsEmpty() ? report.original_to : report.original_rcpt_to;

      if (!complained.IsEmpty() && MessageOrigin::IsHostedAddress(complained))
      {
         // An address this server hosts is not somebody the domain sent to outside;
         // listing it would stop this installation mailing itself.
         LOG_DEBUG(Formatter::Format(_T("Complaints: the feedback report in message {0} names {1}, which this server hosts, so no address is listed for it."),
            message->GetID(), complained));
         complained = String();
      }

      const bool counts = CountsAsComplaint(report.feedback_type);

      ComplaintRow row;
      row.domain_id = domainID;
      row.account_id = AccountIDFor_(sender);
      row.key = ComplaintKey(report.original_message_id, complained, sender, report.arrival_date, message->GetID());
      row.address = complained;
      row.sender = sender;
      row.feedback_type = report.feedback_type;
      row.provider = report.user_agent;
      row.source_ip = report.source_ip;
      row.reported_domain = report.reported_domain;
      row.message_id = report.original_message_id;
      row.subject = report.original_subject;
      row.arrival = report.arrival_date;
      row.received = PersistentBounce::DateTimeFromNow(0);

      const int inserted = PersistentComplaint::Insert(row);

      if (inserted < 0)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6930, "ComplaintTracker::InspectIncomingComplaint",
            Formatter::Format(_T("A feedback report about {0} could not be recorded in the database, so it does not count towards the complaint rate or the suppression list."),
               DomainName(domainID)));
         return;
      }

      if (inserted == 0)
      {
         LOG_DEBUG(Formatter::Format(_T("Complaints: the feedback report in message {0} has already been recorded; it is counted once."),
            message->GetID()));
         return;
      }

      if (!PersistentComplaint::AddToVolume(domainID, PersistentComplaint::Day(0), 0, counts ? 1 : 0))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6932, "ComplaintTracker::InspectIncomingComplaint",
            Formatter::Format(_T("A complaint about {0} was recorded, but the day's tally could not be updated, so the complaint rate will read lower than it is."),
               DomainName(domainID)));
      }

      // The rate has changed, and this is after the write rather than before it: a
      // reader in between would otherwise cache a snapshot without this complaint
      // and answer that for a minute.
      InvalidateRates_();

      LOG_APPLICATION(Formatter::Format(_T("Complaints: a {0} report from {1} about mail {2} sent to {3} was recorded against {4}."),
         report.feedback_type,
         report.user_agent.IsEmpty() ? String(_T("a feedback loop")) : report.user_agent,
         sender.IsEmpty() ? String(_T("(no sender)")) : sender,
         complained.IsEmpty() ? String(_T("a reader the report does not name")) : complained,
         DomainName(domainID)));

      if (!counts || complained.IsEmpty())
         return;

      const int threshold = Configuration::Instance()->GetComplaintSuppressionThreshold();

      const String diagnostic = Formatter::Format(_T("{0} reported by {1}"), report.feedback_type,
         report.user_agent.IsEmpty() ? String(_T("a feedback loop")) : report.user_agent);

      bool listed = false;
      if (BounceTracker::CountComplaint(domainID, complained, threshold, diagnostic, listed) < 0)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6931, "ComplaintTracker::InspectIncomingComplaint",
            Formatter::Format(_T("A complaint about {0} was recorded, but it could not be counted against that address, so it does not count towards the suppression list."),
               complained));
      }
   }

   void
   ComplaintTracker::RecordDeliveryPass(std::shared_ptr<Message> message)
   {
      if (!message || !Enabled_())
         return;

      // With no mailbox named, no complaint can ever arrive, so a denominator would
      // be a write per message for a number nothing reads.
      if (Mailbox_().IsEmpty())
         return;

      const __int64 domainID = BounceTracker::SenderDomainID(message->GetFromAddress());
      if (domainID <= 0)
         return;

      int delivered = 0;
      // owned by this call: the recipients of the message this delivery holds
      for (std::shared_ptr<MessageRecipient> recipient : message->GetRecipients()->GetVector())
      {
         if (recipient->GetDeliveryResult() == MessageRecipient::ResultOK)
            delivered++;
      }

      if (delivered == 0)
         return;

      if (!PersistentComplaint::AddToVolume(domainID, PersistentComplaint::Day(0), delivered, 0))
      {
         ReportTallyUnwritable_(Formatter::Format(_T("{0} recipient(s) of message {1} were not counted for {2}."),
            delivered, message->GetID(), DomainName(domainID)));
      }
   }

   int
   ComplaintTracker::PartsPerMillion(__int64 complaints, __int64 sent)
   {
      if (sent <= 0 || complaints <= 0)
         return 0;

      // Rounded to nearest, in whole parts per million: 0.1% - the line the large
      // providers themselves describe as too high - is 1000 of them.
      const __int64 ppm = (complaints * 1000000 + sent / 2) / sent;

      return (int) (ppm > 1000000 ? 1000000 : ppm);
   }

   bool
   ComplaintTracker::Rates(std::vector<Rate> &rates)
   {
      rates.clear();

      const time_t now = time(nullptr);
      unsigned long generation = 0;

      {
         boost::lock_guard<boost::mutex> guard(rate_mutex_);

         // A clock that went backwards reads as a stale cache, which costs one query.
         if (rate_cache_valid_ && rate_cache_read_at_ <= now && now - rate_cache_read_at_ < RateCacheSeconds)
         {
            rates = rate_cache_;
            return true;
         }

         generation = rate_generation_;
      }

      // Everything below runs with NO lock held. Two callers matter and they are on
      // different threads: the metrics refresher and the alert pass. Holding
      // rate_mutex_ across the query would block the SMTP delivery thread - which
      // takes the same lock to invalidate after writing a complaint - for as long as
      // the database took to answer, and a database that has stopped answering waits
      // DBConnectionAcquireTimeout, sixty seconds by default. A monitoring read must
      // not be able to park mail delivery.
      //
      // The barrier covers the WHOLE read and not just the query: resolving a domain
      // id to its name goes through the domain cache, which falls back to the
      // database on a miss, so it is a second thing that can throw. A read that
      // failed leaves the caller with nothing to report, which is what both callers
      // want: no series and no alert, rather than a rate of zero.
      std::vector<Rate> fresh;

      try
      {
         std::vector<ComplaintVolumeRow> rows;

         if (!PersistentComplaint::ReadVolume(PersistentComplaint::Day(-(RateWindowDays - 1)), rows))
            return false;

         for (const ComplaintVolumeRow &row : rows)
         {
            Rate rate;
            rate.domain_id = row.domain_id;
            rate.domain = DomainName(row.domain_id);

            // A domain deleted since its rows were written has no name. The rows go
            // with it through the foreign key, so this is only ever a domain deleted
            // between the read and here.
            if (rate.domain.IsEmpty())
               continue;

            rate.sent = row.sent;
            rate.complaints = row.complaints;
            rate.judged = row.sent >= RateMinimumVolume;
            rate.parts_per_million = rate.judged ? PartsPerMillion(row.complaints, row.sent) : 0;

            fresh.push_back(rate);
         }

         std::sort(fresh.begin(), fresh.end(), [] (const Rate &a, const Rate &b)
         {
            if (a.parts_per_million != b.parts_per_million)
               return a.parts_per_million > b.parts_per_million;

            return a.domain.Compare(b.domain.c_str()) < 0;
         });
      }
      catch (...)
      {
         return false;
      }

      {
         boost::lock_guard<boost::mutex> guard(rate_mutex_);

         // Published only if nothing was recorded while this was reading. A
         // complaint written in that window is not in `fresh`, and marking it fresh
         // would answer without it for a minute - which is exactly what recording a
         // complaint invalidates the snapshot to prevent.
         if (generation == rate_generation_)
         {
            rate_cache_ = fresh;
            rate_cache_read_at_ = now;
            rate_cache_valid_ = true;
         }
      }

      rates = fresh;
      return true;
   }

   bool
   ComplaintTracker::WorstRate(int thresholdPpm, String &summary)
   {
      summary = String();

      if (thresholdPpm <= 0)
         return false;

      std::vector<Rate> rates;
      if (!Rates(rates))
         return false;

      for (const Rate &rate : rates)
      {
         if (!rate.judged || rate.parts_per_million < thresholdPpm)
            continue;

         summary = Formatter::Format(_T("{0}: {1} complaint(s) in {2} message(s) over the last {3} days, which is {4} in a million."),
            rate.domain, rate.complaints, rate.sent, RateWindowDays, rate.parts_per_million);

         return true;
      }

      return false;
   }

   bool
   ComplaintTracker::ComplaintsJson(__int64 domainID, __int64 accountID, const String &search, int limit, int offset,
                                    AnsiString &json)
   {
      std::vector<ComplaintRow> rows;
      int total = 0;

      if (!PersistentComplaint::List(domainID, accountID, search, limit, offset, rows, total))
         return false;

      // The domain's own rate beside its complaints, because the rate is the number
      // an operator acts on and a page that shows the rows without it shows the
      // denominator nowhere.
      Rate found;
      std::vector<Rate> rates;
      if (Rates(rates))
      {
         for (const Rate &rate : rates)
         {
            if (rate.domain_id == domainID)
            {
               found = rate;
               break;
            }
         }
      }

      // Whether a mailbox is named, not which one. This document is per domain and
      // a key confined to one domain reads it, as does that domain's administrator
      // over COM - and the complaint mailbox is the one thing that tells a genuine
      // report from a forged one, which docs/Complaints.md tells the operator to
      // keep to themselves. A page that needs to say "no mailbox is named, so
      // nothing is ever read" needs the boolean and not the address. The address
      // itself is in the settings group, which is server-wide.
      //
      // suppression_threshold is out for the same reason and a smaller one: it is a
      // server-wide setting that is not this domain's business, and knowing it tells
      // a forger how many reports it takes.
      json = "{" + JsonField("domain", DomainName(domainID)) + "," +
             JsonBool("processing_enabled", Enabled_()) + "," +
             JsonBool("mailbox_configured", !Mailbox_().IsEmpty()) + "," +
             JsonNumber("window_days", RateWindowDays) + "," +
             JsonNumber("sent", found.sent) + "," +
             JsonNumber("complaints", found.complaints) + "," +
             JsonNumber("rate_ppm", found.parts_per_million) + "," +
             JsonBool("rate_judged", found.judged) + "," +
             JsonNumber("total", total) + "," + JsonNumber("offset", offset < 0 ? 0 : offset) + ",\"items\":[";

      for (size_t i = 0; i < rows.size(); i++)
      {
         const ComplaintRow &row = rows[i];

         if (i > 0)
            json += ",";

         json += "{" + JsonField("address", row.address) + "," +
                 JsonField("sender", row.sender) + "," +
                 JsonNumber("account_id", row.account_id) + "," +
                 JsonField("feedback_type", row.feedback_type) + "," +
                 JsonBool("counts", CountsAsComplaint(row.feedback_type)) + "," +
                 JsonField("provider", row.provider) + "," +
                 JsonField("source_ip", row.source_ip) + "," +
                 JsonField("reported_domain", row.reported_domain) + "," +
                 JsonField("message_id", row.message_id) + "," +
                 JsonField("subject", row.subject) + "," +
                 JsonField("arrival", row.arrival) + "," +
                 JsonField("received", row.received) + "}";
      }

      json += "]}";
      return true;
   }

   AnsiString
   ComplaintTracker::RatesJson()
   {
      std::vector<Rate> rates;

      if (!Rates(rates))
         return AnsiString();

      AnsiString json = "{" + JsonBool("processing_enabled", Enabled_()) + "," +
                        JsonField("mailbox", Mailbox_()) + "," +
                        JsonNumber("window_days", RateWindowDays) + "," +
                        JsonNumber("minimum_volume", RateMinimumVolume) + ",\"domains\":[";

      for (size_t i = 0; i < rates.size(); i++)
      {
         if (i > 0)
            json += ",";

         json += "{" + JsonField("domain", rates[i].domain) + "," +
                 JsonNumber("sent", rates[i].sent) + "," +
                 JsonNumber("complaints", rates[i].complaints) + "," +
                 JsonNumber("rate_ppm", rates[i].parts_per_million) + "," +
                 JsonBool("judged", rates[i].judged) + "}";
      }

      json += "]}";
      return json;
   }

   bool
   ComplaintTracker::RunMaintenance(int &complaintsPruned)
   {
      complaintsPruned = 0;

      Configuration *configuration = Configuration::Instance();
      if (!configuration || !configuration->GetSettings())
         return false;

      const int retentionDays = configuration->GetComplaintRetentionDays();

      // The tally never goes below the rate window. The retention is the operator's
      // and may legitimately be short - a complaint names a reader outside this
      // server, and an operator who wants to keep that for a week is entitled to -
      // but the tally holds no address, only a domain, a day and two numbers, and
      // deleting a day that is still inside the window would silently lower every
      // rate this server reports. 0 (keep for ever) stays 0 for both.
      const int tallyRetentionDays = retentionDays <= 0 ? 0
         : (retentionDays < RateWindowDays ? RateWindowDays : retentionDays);

      const int removed = PersistentComplaint::Prune(retentionDays, tallyRetentionDays);
      if (removed < 0)
         return false;

      complaintsPruned = removed;

      // The rates are worked out from rows the sweep has just removed, so the
      // snapshot is not the one to answer the next scrape from.
      if (removed > 0)
         InvalidateRates_();

      return true;
   }

   void
   ComplaintTrackerTester::Test()
   {
      typedef ComplaintTracker C;

      // ------------------------------------------------------- the mailbox test
      Expect(C::IsComplaintMailbox(_T("abuse@example.com"), _T("Abuse@Example.com")),
             "The complaint mailbox was not matched without regard to case.");
      Expect(C::IsComplaintMailbox(_T("fbl@example.com, abuse@example.com"), _T("abuse@example.com")),
             "A comma-separated list of complaint mailboxes was not read.");
      Expect(C::IsComplaintMailbox(_T("fbl@example.com; abuse@example.com"), _T("fbl@example.com")),
             "A semicolon-separated list of complaint mailboxes was not read.");
      Expect(C::IsComplaintMailbox(_T("fbl@example.com abuse@example.com"), _T("abuse@example.com")),
             "A space-separated list of complaint mailboxes was not read.");
      Expect(!C::IsComplaintMailbox(_T("abuse@example.com"), _T("somebody@example.com")),
             "An ordinary mailbox was taken for the complaint mailbox.");
      Expect(!C::IsComplaintMailbox(_T(""), _T("abuse@example.com")),
             "Every address was taken for the complaint mailbox when none is configured.");
      Expect(!C::IsComplaintMailbox(_T("abuse@example.com"), _T("")),
             "An empty address matched the complaint mailbox.");

      // ---------------------------------------------------------- the types
      Expect(C::CountsAsComplaint(_T("abuse")), "An abuse report was not counted as a complaint.");
      Expect(C::CountsAsComplaint(_T("FRAUD")), "A feedback type was not read without regard to case.");
      Expect(C::CountsAsComplaint(_T("virus")), "A virus report was not counted as a complaint.");
      Expect(C::CountsAsComplaint(_T("something-new")), "An unregistered feedback type was not counted as a complaint.");
      Expect(!C::CountsAsComplaint(_T("not-spam")), "not-spam was counted as a complaint.");
      Expect(!C::CountsAsComplaint(_T("")), "An absent feedback type was counted as a complaint.");

      // ------------------------------------------------------------- the key
      const String a = C::ComplaintKey(_T("<ABC@example.com>"), _T("Reader@Provider.test"), _T("sender@example.com"), _T("2026-09-16 10:00:00"), 11);
      const String b = C::ComplaintKey(_T("abc@example.com"), _T("reader@provider.test"), _T("SENDER@example.com"), _T("2026-09-16 10:00:00"), 22);
      Expect(a == b, "The same report read twice did not produce the same key.");
      Expect(a.GetLength() == 64, "The complaint key is not a SHA-256 in hex.");

      const String other = C::ComplaintKey(_T("<def@example.com>"), _T("reader@provider.test"), _T("sender@example.com"), _T("2026-09-16 10:00:00"), 11);
      Expect(a != other, "Two reports about different messages shared a key.");

      // Nothing identifies the message: the local message's id keeps them apart.
      const String blindOne = C::ComplaintKey(_T(""), _T(""), _T("sender@example.com"), _T(""), 11);
      const String blindTwo = C::ComplaintKey(_T(""), _T(""), _T("sender@example.com"), _T(""), 12);
      Expect(blindOne != blindTwo, "Two reports naming no message at all shared a key, so one would be lost.");

      // An arrival time alone is enough to tell two reports apart, so a repeat of
      // one of them is still recognised.
      const String datedOne = C::ComplaintKey(_T(""), _T("reader@provider.test"), _T("sender@example.com"), _T("2026-09-16 10:00:00"), 11);
      const String datedTwo = C::ComplaintKey(_T(""), _T("reader@provider.test"), _T("sender@example.com"), _T("2026-09-16 10:00:00"), 99);
      Expect(datedOne == datedTwo, "A repeat of a report that carries an arrival date was not recognised.");

      // ------------------------------------------------------------ the rate
      Expect(C::PartsPerMillion(0, 1000) == 0, "No complaints did not read as a rate of zero.");
      Expect(C::PartsPerMillion(1, 1000) == 1000, "One complaint in a thousand is not 1000 in a million.");
      Expect(C::PartsPerMillion(1, 1000000) == 1, "One complaint in a million is not 1 in a million.");
      Expect(C::PartsPerMillion(5, 10000) == 500, "Five complaints in ten thousand is not 500 in a million.");
      Expect(C::PartsPerMillion(3, 1000000) == 3, "Three complaints in a million is not 3 in a million.");
      Expect(C::PartsPerMillion(1, 3) == 333333, "A rate was not rounded to the nearest part per million.");
      Expect(C::PartsPerMillion(2, 3) == 666667, "A rate was not rounded to the nearest part per million.");
      Expect(C::PartsPerMillion(1, 0) == 0, "A rate with nothing sent was not zero.");
      Expect(C::PartsPerMillion(10, 1) == 1000000, "A rate above one in one was not held at a million.");
   }
}
