// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"

#include "BounceTracker.h"
#include "DeliveryReportParser.h"

#include "../Common/AntiSpam/AuthenticationResults.h"
#include "../Common/Application/ObjectCache.h"
#include "../Common/BO/Domain.h"
#include "../Common/BO/DomainAliases.h"
#include "../Common/BO/Message.h"
#include "../Common/BO/MessageRecipient.h"
#include "../Common/BO/MessageRecipients.h"
#include "../Common/Cache/CacheContainer.h"
#include "../Common/Persistence/PersistentBounce.h"
#include "../Common/Persistence/PersistentMessage.h"
#include "../Common/Persistence/PersistentMessageRecipient.h"
#include "../Common/Scripting/Events.h"
#include "../Common/Util/AWStats.h"
#include "../Common/Util/BATV.h"
#include "../Common/Util/Hashing/HashCreator.h"
#include "../Common/Util/MessageOrigin.h"
#include "../Common/Util/MessageTrace.h"
#include "../Common/Util/SRS.h"
#include "../Common/Util/Unicode.h"
#include "../Common/Util/Utilities.h"

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
      // One writer of an address's count row at a time in this process: a bounce
      // being counted, an acceptance setting the run aside, an administrator's
      // removal. Two deliveries bouncing off the same address at once would
      // otherwise both read a count of 0 and both insert, and a removal landing
      // between a count's read and its write would be written over. Striped by
      // address, so deliveries to different addresses do not wait on each other.
      // Across processes - two nodes on one database - the unique index turns the
      // insert race into an insert that loses, which Count_ reads back and adds to,
      // and the counters are added to in SQL.
      const size_t CountLockStripes = 64;
      boost::mutex count_mutexes_[CountLockStripes];

      boost::mutex &CountLockFor(const String &address)
      {
         const String canonical = PersistentBounce::Canonical(address);

         size_t hash = 5381;
         for (int i = 0; i < canonical.GetLength(); i++)
            hash = (hash * 33) ^ (size_t) canonical.GetAt(i);

         return count_mutexes_[hash % CountLockStripes];
      }

      boost::mutex unreadable_mutex_;
      time_t unreadable_reported_at_ = 0;
      const time_t UnreadableReportIntervalSeconds = 3600;

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

      bool WantsFailureNotice(std::shared_ptr<MessageRecipient> recipient)
      {
         const int notify = recipient->GetDSNNotify();
         return notify == MessageRecipient::DSNNotifyDefault || (notify & MessageRecipient::DSNNotifyFailure) != 0;
      }

      void Expect(bool condition, const char *message)
      {
         if (!condition)
            throw std::logic_error(message);
      }
   }

   const char *
   BounceTracker::ActionName(int action)
   {
      switch (action)
      {
      case ActionRefuse:  return "refuse";
      case ActionDrop:    return "drop";
      case ActionObserve: return "observe";
      }

      return "refuse";
   }

   bool
   BounceTracker::Enabled_()
   {
      Configuration *configuration = Configuration::Instance();
      return configuration && configuration->GetSettings() && configuration->GetBounceProcessingEnabled();
   }

   bool
   BounceTracker::ListApplies_()
   {
      Configuration *configuration = Configuration::Instance();
      if (!configuration || !configuration->GetSettings())
         return false;

      return configuration->GetBounceProcessingEnabled() || configuration->GetComplaintProcessingEnabled();
   }

   __int64
   BounceTracker::SenderDomainID(const String &sender)
   {
      if (sender.IsEmpty())
         return 0;

      // A forward this server rewrote into SRS belongs to the original sender, not
      // to anybody here, and a prvs address is a return path rather than a sender.
      if (SRS::IsSRS0(sender) || BATV::IsPrvs(sender))
         return 0;

      String address = sender;

      std::shared_ptr<DomainAliases> domainAliases = ObjectCache::Instance()->GetDomainAliases();
      if (domainAliases)
         address = domainAliases->ApplyAliasesOnAddress(address);

      const String domainName = StringParser::ExtractDomain(address);
      if (domainName.IsEmpty())
         return 0;

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(domainName);
      return domain ? domain->GetID() : 0;
   }

   __int64
   BounceTracker::DomainIDByName(const String &domainName)
   {
      String name = domainName;
      name.Trim();

      if (name.IsEmpty())
         return 0;

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(name);
      return domain ? domain->GetID() : 0;
   }

   void
   BounceTracker::ReportListUnreadable_(const String &detail)
   {
      {
         boost::lock_guard<boost::mutex> guard(unreadable_mutex_);

         const time_t now = time(nullptr);

         if (unreadable_reported_at_ != 0 && unreadable_reported_at_ <= now &&
             now - unreadable_reported_at_ < UnreadableReportIntervalSeconds)
            return;

         unreadable_reported_at_ = now;
      }

      ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6622, "BounceTracker",
         "The suppression list could not be read, so mail was sent as if the address were not listed. " + detail +
         " This is reported at most once an hour while it lasts.");
   }

   String
   BounceTracker::ReportKey(const String &messageId, const String &recipient, const String &sender)
   {
      const String material = DeliveryReportParser::NormaliseMessageId(messageId) + _T("\n") +
                              PersistentBounce::Canonical(recipient) + _T("\n") +
                              PersistentBounce::Canonical(sender);

      HashCreator hasher(HashCreator::SHA256);
      return String(hasher.GenerateHashNoSalt(Utf8(material), HashCreator::hex));
   }

   bool
   BounceTracker::AcceptReport(const BounceReportFacts &facts, String &reason)
   {
      if (!facts.envelope_sender_empty)
      {
         reason = _T("its envelope sender is not null, so it is not a delivery report");
         return false;
      }

      if (facts.spam_flagged)
      {
         reason = _T("it was classified as spam");
         return false;
      }

      if (!facts.received_by_this_server)
      {
         reason = _T("it did not arrive over SMTP at this server - this server's own reports carry no Received field of its own");
         return false;
      }

      if (facts.received_authenticated)
      {
         reason = _T("it was submitted over an authenticated session, which a remote MTA's report never is");
         return false;
      }

      if (facts.auth_results_spf_fail)
      {
         reason = _T("this server's Authentication-Results says SPF failed");
         return false;
      }

      if (facts.auth_results_dmarc_fail)
      {
         reason = _T("this server's Authentication-Results says DMARC failed");
         return false;
      }

      if (facts.batv_enabled && !facts.recipient_is_verified_prvs)
      {
         reason = _T("BATV is on and it was not addressed to a prvs return path that verifies");
         return false;
      }

      reason = _T("it passed every check");
      return true;
   }

   BounceClassifier::Verdict
   BounceTracker::ApplySessionGuard(const BounceClassifier::Verdict &verdict, int identicalRefusals)
   {
      if (identicalRefusals <= 1)
         return verdict;

      if (verdict.category == BounceNone || verdict.category == BounceSoft || verdict.category == BouncePolicy)
         return verdict;

      BounceClassifier::Verdict guarded = verdict;
      guarded.category = (verdict.status.GetLength() >= 3 && verdict.status.Mid(1, 3) == _T(".7.")) ? BouncePolicy : BounceOther;
      return guarded;
   }

   int
   BounceTracker::ReportRunIncrease(int increment, int setAside, int laterKey, const String &lastDelivered, const String &sent)
   {
      if (laterKey != 0)
         return 0;

      // Both timestamps come from the one clock, in the one sortable spelling, and
      // this message's own acceptance wrote the same value to both - so "later"
      // is strictly later.
      if (!lastDelivered.IsEmpty() && !sent.IsEmpty() && lastDelivered.Compare(sent.c_str()) > 0)
         return 0;

      return (setAside > 0 ? setAside : 0) + (increment > 0 ? increment : 0);
   }

   std::vector<size_t>
   BounceTracker::ReportedFailures(const DeliveryReport &report, size_t limit, bool &truncated)
   {
      std::vector<size_t> indexes;
      std::set<String> seen;
      truncated = false;

      for (size_t i = 0; i < report.recipients.size(); i++)
      {
         const DeliveryReportRecipient &reported = report.recipients[i];

         // delayed, delivered, relayed and expanded are not bounces.
         if (reported.action != _T("failed"))
            continue;

         const String canonical = PersistentBounce::Canonical(reported.final_recipient);
         if (canonical.IsEmpty() || seen.find(canonical) != seen.end())
            continue;

         if (indexes.size() >= limit)
         {
            truncated = true;
            break;
         }

         seen.insert(canonical);
         indexes.push_back(i);
      }

      return indexes;
   }

   void
   BounceTracker::Count_(__int64 domainID, const String &address, const BounceClassifier::Verdict &verdict,
                         int source, const String &diagnostic, const String &remoteMta,
                         const BounceReportKeyRow *reportKey)
   {
      if (domainID <= 0 || verdict.category == BounceNone)
         return;

      const String canonical = PersistentBounce::Canonical(address);
      if (canonical.IsEmpty())
         return;

      Configuration *configuration = Configuration::Instance();
      const bool mailboxFullIsHard = configuration->GetBounceMailboxFullIsHard();
      const int threshold = configuration->GetBounceHardBounceThreshold();
      const bool suppresses = BounceClassifier::Suppresses(verdict.category, mailboxFullIsHard);
      const String now = PersistentBounce::DateTimeFromNow(0);

      int consecutive = 0;

      {
         // Every writer of the run in this process takes this lock: the counting
         // here, an acceptance setting the run aside, an administrator's removal.
         // Across nodes the counters themselves are added to in SQL, so no count is
         // lost; only which of two simultaneous events lands first is left open.
         boost::lock_guard<boost::mutex> guard(CountLockFor(canonical));

         // Twice at most: the second time is the insert that lost a race with
         // another node, read back and applied to the row that node wrote.
         bool counted = false;
         bool runDecided = false;
         int addToRun = 0;

         for (int attempt = 0; attempt < 2 && !counted; attempt++)
         {
            BounceCountRow row;
            const int found = PersistentBounce::FindCount(domainID, canonical, row);

            if (found < 0)
               break;

            if (!runDecided)
            {
               runDecided = true;
               const int increment = suppresses ? 1 : 0;

               if (!reportKey)
               {
                  addToRun = increment;
               }
               else
               {
                  __int64 laterID = 0;
                  const int later = PersistentBounce::FindLaterReportKey(domainID, reportKey->address_key, reportKey->id, laterID);

                  addToRun = ReportRunIncrease(increment, reportKey->run, later, found == 1 ? row.last_delivered : String(), reportKey->sent);

                  if (later == 1 && !PersistentBounce::AddToReportKeyRun(laterID, reportKey->run + increment))
                  {
                     ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6620, "BounceTracker::Count_",
                        Formatter::Format(_T("A delivery report for {0} could not carry its run of hard bounces forward to the later message to that address, so the run may come out short."),
                           canonical));
                  }
               }
            }

            if (found == 0)
            {
               row.domain_id = domainID;
               row.address = canonical;

               switch (verdict.category)
               {
               case BounceHard:        row.hard = 1; break;
               case BounceSoft:        row.soft = 1; break;
               case BounceMailboxFull: row.mailbox_full = 1; break;
               case BouncePolicy:      row.policy = 1; break;
               default:                row.other = 1; break;
               }

               row.consecutive_hard = addToRun;
               row.last_status = verdict.status;
               row.last_category = (int) verdict.category;
               row.last_bounce = now;

               counted = PersistentBounce::InsertCount(row);
               consecutive = row.consecutive_hard;

               // A lost insert is a race with another node: read back and added to.
               continue;
            }

            counted = PersistentBounce::AddToCount(row.id, (int) verdict.category, addToRun, verdict.status, now);
            consecutive = row.consecutive_hard + addToRun;

            // An update that failed is not a race; only a lost insert is retried.
            if (!counted)
               break;
         }

         if (!counted)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6620, "BounceTracker::Count_",
               Formatter::Format(_T("A {0} bounce for {1} could not be counted in the database, so it will not count towards the suppression list."),
                  String(BounceClassifier::Name(verdict.category)), canonical));
            return;
         }
      }

      LOG_DEBUG(Formatter::Format(_T("Bounces: counted a {0} bounce ({1}) for {2} against domain {3}; {4} in a row."),
         String(BounceClassifier::Name(verdict.category)), verdict.status, canonical, domainID, consecutive));

      if (!suppresses || consecutive < threshold)
         return;

      BounceSuppressionEntry existing;
      const int listed = PersistentBounce::FindSuppression(domainID, canonical, existing);

      if (listed < 0)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6620, "BounceTracker::Count_",
            Formatter::Format(_T("{0} has bounced hard {1} time(s) in a row, but the suppression list could not be read, so it has not been listed."),
               canonical, consecutive));
         return;
      }

      if (listed == 1)
      {
         // Listed already. An entry that has expired but not yet been swept is
         // replaced, so a fresh bounce starts a fresh entry rather than none.
         if (!PersistentBounce::IsExpired(existing, configuration->GetBounceSuppressionExpiryDays(), now))
            return;

         PersistentBounce::DeleteSuppression(domainID, canonical);
      }

      BounceSuppressionEntry entry;
      entry.domain_id = domainID;
      entry.address = canonical;
      entry.source = source;
      entry.category = (int) verdict.category;
      entry.status = verdict.status;
      entry.diagnostic = diagnostic;
      entry.remote_mta = remoteMta;
      entry.created = now;

      if (!PersistentBounce::InsertSuppression(entry))
      {
         BounceSuppressionEntry raced;
         if (PersistentBounce::FindSuppression(domainID, canonical, raced) != 1)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6620, "BounceTracker::Count_",
               Formatter::Format(_T("{0} has bounced hard {1} time(s) in a row and should be on the suppression list, but the entry could not be written."),
                  canonical, consecutive));
         }

         return;
      }

      LOG_APPLICATION(Formatter::Format(_T("Bounces: {0} was put on the suppression list of {1} after {2} hard bounce(s) in a row ({3}, from a {4})."),
         canonical, DomainName(domainID), consecutive, verdict.status, String(PersistentBounce::SourceName(source))));
   }

   int
   BounceTracker::CountComplaint(__int64 domainID, const String &address, int threshold,
                                 const String &diagnostic, bool &listed)
   {
      listed = false;

      const String canonical = PersistentBounce::Canonical(address);
      if (domainID <= 0 || canonical.IsEmpty())
         return -1;

      const String now = PersistentBounce::DateTimeFromNow(0);
      int complaints = 0;

      {
         // The same stripe every other writer of this address's row takes.
         boost::lock_guard<boost::mutex> guard(CountLockFor(canonical));

         // Twice at most: the second time is the insert that lost a race with
         // another node, read back and applied to the row that node wrote.
         bool counted = false;

         for (int attempt = 0; attempt < 2 && !counted; attempt++)
         {
            BounceCountRow row;
            const int found = PersistentBounce::FindCount(domainID, canonical, row);

            if (found < 0)
               break;

            if (found == 0)
            {
               // No bounce has ever been counted for this address. The row exists
               // from now on with every bounce counter at zero, which is the truth:
               // the mail arrived, and somebody did not want it.
               //
               // bclastdelivered is deliberately left empty. It means "a remote
               // accepted a message to this address", it is the column
               // ReportRunIncrease reads to decide whether a delivery report that
               // arrives later still adds to the run of hard bounces, and a
               // complaint is not a delivery: writing the report's own read time
               // there would make a genuine hard bounce for an earlier message
               // count for nothing. The complaint's recency goes in its own column.
               row.domain_id = domainID;
               row.address = canonical;
               row.complaints = 1;
               row.last_category = (int) BounceNone;
               row.last_complaint = now;

               counted = PersistentBounce::InsertCount(row);
               complaints = 1;
               continue;
            }

            counted = PersistentBounce::AddComplaintToCount(row.id, now);
            complaints = row.complaints + 1;

            // An update that failed is not a race; only a lost insert is retried.
            if (!counted)
               break;
         }

         if (!counted)
            return -1;
      }

      LOG_DEBUG(Formatter::Format(_T("Complaints: counted a complaint for {0} against domain {1}; {2} in all."),
         canonical, domainID, complaints));

      if (threshold <= 0 || complaints < threshold)
         return complaints;

      BounceSuppressionEntry existing;
      const int found = PersistentBounce::FindSuppression(domainID, canonical, existing);

      if (found < 0)
         return -1;

      if (found == 1)
      {
         // Listed already, by a bounce, a report, an administrator or an earlier
         // complaint. An entry that has expired but not been swept is replaced, so
         // a fresh complaint starts a fresh entry rather than none - the rule
         // Count_ applies to a fresh bounce.
         if (!PersistentBounce::IsExpired(existing, Configuration::Instance()->GetBounceSuppressionExpiryDays(), now))
            return complaints;

         PersistentBounce::DeleteSuppression(domainID, canonical);
      }

      BounceSuppressionEntry entry;
      entry.domain_id = domainID;
      entry.address = canonical;
      entry.source = PersistentBounce::SourceComplaint;
      entry.category = (int) BounceNone;
      entry.diagnostic = diagnostic;
      entry.created = now;

      if (!PersistentBounce::InsertSuppression(entry))
      {
         BounceSuppressionEntry raced;
         if (PersistentBounce::FindSuppression(domainID, canonical, raced) != 1)
            return -1;

         return complaints;
      }

      listed = true;

      LOG_APPLICATION(Formatter::Format(_T("Complaints: {0} was put on the suppression list of {1} after {2} complaint(s)."),
         canonical, DomainName(domainID), complaints));

      return complaints;
   }

   void
   BounceTracker::RecordDeliveryPass(std::shared_ptr<Message> message, bool messageRescheduled)
   {
      if (!message || !Enabled_())
         return;

      const std::vector<std::shared_ptr<MessageRecipient> > &recipients = message->GetRecipients()->GetVector();
      if (recipients.empty())
         return;

      const __int64 domainID = SenderDomainID(message->GetFromAddress());
      if (domainID <= 0)
         return;

      const bool readReports = Configuration::Instance()->GetBounceReadDeliveryReports();

      // The session-level guard's tally: how many recipients of this message were
      // refused by the same host in the same words.
      std::map<String, int> identicalRefusals;
      for (std::shared_ptr<MessageRecipient> recipient : recipients)
      {
         if (recipient->GetDeliveryResult() == MessageRecipient::ResultFatalError && !recipient->GetRemoteSmtpReply().IsEmpty())
            identicalRefusals[recipient->GetRemoteMta() + _T("\n") + recipient->GetRemoteSmtpReply()]++;
      }

      String messageId;
      bool messageIdRead = false;

      for (std::shared_ptr<MessageRecipient> recipient : recipients)
      {
         switch (recipient->GetDeliveryResult())
         {
         case MessageRecipient::ResultOK:
            {
               // Once per message, and only now that a remote has accepted it: the
               // Message-ID is in the file, which is still there until the queue
               // row goes after this pass.
               if (readReports && !messageIdRead)
               {
                  messageIdRead = true;

                  const AnsiString header = PersistentMessage::LoadHeader(PersistentMessage::GetFileName(message), false);
                  const AnsiString value = MessageOrigin::FirstFieldValue(header, "Message-ID");

                  String wide;
                  if (Unicode::MultiByteToWide(value, wide))
                     messageId = DeliveryReportParser::NormaliseMessageId(wide);
               }

               // A message without a Message-ID cannot be matched later, so no key
               // is kept for it and its acceptance is a delivery outright.
               const String key = (readReports && !messageId.IsEmpty())
                  ? ReportKey(messageId, recipient->GetAddress(), message->GetFromAddress())
                  : String();

               RecordAcceptance_(domainID, message, recipient->GetAddress(), key);
            }
            break;

         case MessageRecipient::ResultFatalError:
            {
               BounceClassifier::Verdict verdict =
                  BounceClassifier::ClassifyRefusal(recipient->GetEnhancedStatusCode(), recipient->GetRemoteSmtpReply());

               if (!recipient->GetRemoteSmtpReply().IsEmpty())
               {
                  verdict = ApplySessionGuard(verdict,
                     identicalRefusals[recipient->GetRemoteMta() + _T("\n") + recipient->GetRemoteSmtpReply()]);
               }

               Count_(domainID, recipient->GetAddress(), verdict, PersistentBounce::SourceBounce,
                  recipient->GetRemoteSmtpReply(), recipient->GetRemoteMta(), nullptr);
            }
            break;

         default:
            {
               // A temporary failure is counted once, when this server gives up on
               // it, and never while it is still being retried.
               if (messageRescheduled)
                  break;

               const String status = recipient->GetEnhancedStatusCode().IsEmpty() ? String(_T("4.4.2")) : recipient->GetEnhancedStatusCode();

               BounceClassifier::Verdict verdict = BounceClassifier::ClassifyRefusal(status, recipient->GetRemoteSmtpReply());

               // Given up on, but never proven dead: a temporary condition that
               // outlasted the retries is soft whatever its words said.
               if (verdict.category == BounceHard || verdict.category == BounceOther || verdict.category == BounceNone ||
                   verdict.category == BouncePolicy)
                  verdict.category = BounceSoft;

               Count_(domainID, recipient->GetAddress(), verdict, PersistentBounce::SourceBounce,
                  recipient->GetRemoteSmtpReply(), recipient->GetRemoteMta(), nullptr);
            }
            break;
         }
      }
   }

   void
   BounceTracker::RecordAcceptance_(__int64 domainID, std::shared_ptr<Message> message, const String &address, const String &reportKey)
   {
      // One timestamp for the count row's last delivery and the register row's
      // time sent, so a report can tell this acceptance from a later one exactly.
      const String now = PersistentBounce::DateTimeFromNow(0);

      boost::lock_guard<boost::mutex> guard(CountLockFor(address));

      BounceCountRow row;
      const int found = PersistentBounce::FindCount(domainID, address, row);

      // With a key, the run is set aside in the register rather than ended: the
      // report that this message failed after all may still come, and gives it
      // back (ReportRunIncrease). Without one, nothing can give it back, and the
      // acceptance is a delivery outright. A count that could not be read sets
      // nothing aside, which can only make a later run short.
      const int setAside = found == 1 ? row.consecutive_hard : 0;

      // An address that has never bounced has no row to update. One whose row could
      // not be read is updated anyway: the update may get through where the read
      // did not, and if it does not, its failure is the error worth reporting.
      if (found != 0 && !PersistentBounce::RecordDelivered(domainID, address, now))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6625, "BounceTracker::RecordDeliveryPass",
            Formatter::Format(_T("Message {0} was delivered to {1}, but that could not be recorded against its bounce count, so its run of hard bounces was not ended and a later hard bounce may continue it."),
               message->GetID(), address));
      }

      if (reportKey.IsEmpty())
         return;

      if (!PersistentBounce::InsertReportKey(domainID, reportKey, PersistentBounce::AddressKey(address), setAside, now))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6621, "BounceTracker::RecordDeliveryPass",
            Formatter::Format(_T("Message {0} was delivered to {1}, but its report key could not be recorded, so a delivery report that later arrives for it will be ignored."),
               message->GetID(), address));
      }
   }

   void
   BounceTracker::InspectIncomingReport(std::shared_ptr<Message> message)
   {
      if (!message || !Enabled_())
         return;

      // The cheapest test first: nearly every message has a sender, and a report
      // never does.
      if (!message->GetFromAddress().IsEmpty())
         return;

      if (!Configuration::Instance()->GetBounceReadDeliveryReports())
         return;

      // The local recipient, by the address the report was sent to: the RCPT
      // address as given, which for a BATV-signed return path is the prvs form. An
      // alias that expands to several accounts is still one address here.
      std::vector<String> localAddresses;
      std::set<String> distinctAddresses;
      for (std::shared_ptr<MessageRecipient> recipient : message->GetRecipients()->GetVector())
      {
         if (recipient->GetLocalAccountID() <= 0)
            continue;

         const String original = recipient->GetOriginalAddress().IsEmpty() ? recipient->GetAddress() : recipient->GetOriginalAddress();
         if (distinctAddresses.insert(PersistentBounce::Canonical(original)).second)
            localAddresses.push_back(original);
      }

      if (localAddresses.empty())
         return;

      // A report goes to the one envelope sender of the message it reports on. A
      // null-sender message to several of this server's addresses is not one, and
      // reading it once per address would let one message cost this server a
      // register lookup per address per recipient it names.
      if (localAddresses.size() > 1)
      {
         LOG_DEBUG(Formatter::Format(_T("Bounces: message {0} has a null sender but was addressed to {1} of this server's addresses; a delivery report goes to one, so it is not read as one."),
            message->GetID(), (int) localAddresses.size()));
         return;
      }

      const String fileName = PersistentMessage::GetFileName(message);
      const AnsiString header = PersistentMessage::LoadHeader(fileName, false);

      BounceReportFacts facts;
      facts.envelope_sender_empty = true;
      facts.spam_flagged = message->GetFlagSpam();

      const String computerName = Utilities::ComputerName();
      facts.received_by_this_server = MessageOrigin::ReceivedByThisServer(header, computerName);
      facts.received_authenticated = MessageOrigin::ReceivedSaysAuthenticated(header, computerName);

      // Only the Authentication-Results this server wrote: its authserv-id first.
      // The writer strips any field claiming that id from what arrives, so one
      // that carries it is ours.
      AnsiString authResults = MessageOrigin::FirstFieldValue(header, "Authentication-Results");
      authResults.ToLower();
      authResults.Trim();
      const AnsiString authservId = AuthenticationResults::GetAuthservId();
      if (!authservId.IsEmpty() && authResults.Find(authservId) == 0)
      {
         facts.auth_results_spf_fail = authResults.Find("spf=fail") >= 0;
         facts.auth_results_dmarc_fail = authResults.Find("dmarc=fail") >= 0;
      }

      facts.batv_enabled = IniFileSettings::Instance()->GetBATVEnabled();

      DeliveryReport report;
      bool reportRead = false;

      for (const String &localAddress : localAddresses)
      {
         String sender = localAddress;

         facts.recipient_is_verified_prvs = false;
         if (BATV::IsPrvs(localAddress))
         {
            String stripped;
            if (BATV::Verify(localAddress, IniFileSettings::Instance()->GetBATVSecret(), stripped))
            {
               facts.recipient_is_verified_prvs = true;
               sender = stripped;
            }
         }

         String reason;
         if (!AcceptReport(facts, reason))
         {
            LOG_DEBUG(Formatter::Format(_T("Bounces: message {0} to {1} is not read as a delivery report: {2}."),
               message->GetID(), localAddress, reason));
            continue;
         }

         if (!reportRead)
         {
            reportRead = true;

            if (!DeliveryReportParser::ParseFile(fileName, report))
            {
               LOG_DEBUG(Formatter::Format(_T("Bounces: message {0} has a null sender but is not an RFC 3464 delivery report this server can read."),
                  message->GetID()));
               return;
            }

            if (report.original_message_id.IsEmpty())
            {
               LOG_DEBUG(Formatter::Format(_T("Bounces: the delivery report in message {0} does not carry the original Message-ID, so it cannot be matched to a message this server sent."),
                  message->GetID()));
               return;
            }
         }

         bool truncated = false;
         const std::vector<size_t> failures = ReportedFailures(report, MaxReportLookups, truncated);

         if (truncated)
         {
            LOG_DEBUG(Formatter::Format(_T("Bounces: the delivery report in message {0} names more than {1} failed recipients; only the first {1} are looked up."),
               message->GetID(), (int) MaxReportLookups));
         }

         for (size_t index : failures)
         {
            const DeliveryReportRecipient &reported = report.recipients[index];

            BounceReportKeyRow keyRow;
            const int consumed = PersistentBounce::ConsumeReportKey(
               ReportKey(report.original_message_id, reported.final_recipient, sender), keyRow);

            if (consumed < 0)
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6620, "BounceTracker::InspectIncomingReport",
                  Formatter::Format(_T("A delivery report about {0} could not be matched against the register of sent messages, so it has not been counted."),
                     reported.final_recipient));
               continue;
            }

            if (consumed == 0)
            {
               LOG_DEBUG(Formatter::Format(_T("Bounces: the delivery report in message {0} names {1}, which does not match a message this server sent to it for {2}; ignored."),
                  message->GetID(), reported.final_recipient, sender));
               continue;
            }

            const BounceClassifier::Verdict verdict =
               BounceClassifier::ClassifyReport(reported.status, reported.diagnostic, reported.diagnostic_type);

            Count_(keyRow.domain_id, reported.final_recipient, verdict, PersistentBounce::SourceReport,
               reported.diagnostic, reported.remote_mta, &keyRow);
         }
      }
   }

   String
   BounceTracker::DescribeEntry_(const BounceSuppressionEntry &entry, const String &domainName, String &enhancedCode)
   {
      const String since = entry.created.Left(10);

      if (entry.source == PersistentBounce::SourceManual)
      {
         enhancedCode = _T("5.7.1");
         return Formatter::Format(_T("{0} is on the suppression list of {1}: it was added by an administrator on {2}. An administrator can remove it."),
            entry.address, domainName, since);
      }

      if (entry.source == PersistentBounce::SourceComplaint)
      {
         // Not a dead mailbox: somebody there read the mail and reported it as
         // spam, which is 5.7.1 - refused on policy - and never 5.1.1.
         enhancedCode = _T("5.7.1");
         return Formatter::Format(_T("{0} is on the suppression list of {1}: its provider reported mail to it as spam on {2}. An administrator can remove it."),
            entry.address, domainName, since);
      }

      enhancedCode = BounceClassifier::IsEnhancedStatusCode(entry.status) && entry.status.GetAt(0) == '5' ? entry.status : String(_T("5.7.1"));

      return Formatter::Format(_T("{0} is on the suppression list of {1}: mail to it bounced permanently ({2}) on {3}. An administrator can remove it."),
         entry.address, domainName, entry.status, since);
   }

   bool
   BounceTracker::RefuseAtSubmission(const String &sender, const String &recipient, String &enhancedCode, String &text)
   {
      String source;
      return RefuseAtSubmission(sender, recipient, enhancedCode, text, source);
   }

   bool
   BounceTracker::RefuseAtSubmission(const String &sender, const String &recipient, String &enhancedCode, String &text, String &source)
   {
      if (!ListApplies_())
         return false;

      if (Configuration::Instance()->GetBounceSuppressionAction() != ActionRefuse)
         return false;

      const __int64 domainID = SenderDomainID(sender);
      if (domainID <= 0)
         return false;

      // A hosted address is never listed: it is delivered here, and nothing about
      // a remote server's refusal applies to it.
      if (MessageOrigin::IsHostedAddress(recipient))
         return false;

      BounceSuppressionEntry entry;
      const int found = PersistentBounce::FindSuppression(domainID, recipient, entry);

      if (found < 0)
      {
         ReportListUnreadable_(Formatter::Format(_T("The recipient was {0}, submitted by {1}."), recipient, sender));
         return false;
      }

      if (found == 0)
         return false;

      const String now = PersistentBounce::DateTimeFromNow(0);
      if (PersistentBounce::IsExpired(entry, Configuration::Instance()->GetBounceSuppressionExpiryDays(), now))
         return false;

      text = DescribeEntry_(entry, DomainName(domainID), enhancedCode);
      source = String(PersistentBounce::SourceName(entry.source));

      LOG_APPLICATION(Formatter::Format(_T("Bounces: refused {0} for {1}: the address is on the suppression list."), recipient, sender));

      return true;
   }

   void
   BounceTracker::ApplyAtDelivery(std::shared_ptr<Message> message, const String &sendersIP, std::vector<DeliveryFailure> &failures)
   {
      if (!message || !ListApplies_())
         return;

      std::vector<std::shared_ptr<MessageRecipient> > &recipients = message->GetRecipients()->GetVector();
      if (recipients.empty())
         return;

      const String sender = message->GetFromAddress();
      const __int64 domainID = SenderDomainID(sender);
      if (domainID <= 0)
         return;

      Configuration *configuration = Configuration::Instance();
      const int action = configuration->GetBounceSuppressionAction();
      const int expiryDays = configuration->GetBounceSuppressionExpiryDays();
      const String now = PersistentBounce::DateTimeFromNow(0);
      const String domainName = DomainName(domainID);

      auto iterator = recipients.begin();
      while (iterator != recipients.end())
      {
         std::shared_ptr<MessageRecipient> recipient = *iterator;
         const String address = recipient->GetAddress();

         BounceSuppressionEntry entry;
         const int found = PersistentBounce::FindSuppression(domainID, address, entry);

         if (found < 0)
         {
            ReportListUnreadable_(Formatter::Format(_T("Message {0} to {1} was delivered without the check."), message->GetID(), address));
            iterator++;
            continue;
         }

         if (found == 0 || PersistentBounce::IsExpired(entry, expiryDays, now))
         {
            iterator++;
            continue;
         }

         String enhancedCode;
         const String description = DescribeEntry_(entry, domainName, enhancedCode);

         if (action == ActionObserve)
         {
            LOG_APPLICATION(Formatter::Format(_T("SMTPDeliverer - Message {0}: {1} (BounceSuppressionAction is observe, so it is delivered.)"),
               message->GetID(), description));

            MessageTrace::Record(message->GetID(), MessageTrace::EventSuppressed, sender, address, sendersIP, 250,
               _T("observe: on the suppression list, delivered"));

            iterator++;
            continue;
         }

         // Gone from this pass either way. A row that could not be deleted goes with
         // the message's queue row only if nothing else keeps the message queued.
         // When another recipient is deferred, the message is rescheduled, its
         // recipients are read back from the database on the next attempt, and this
         // one meets the list again - refused and reported to the sender again, or
         // dropped again. The error says so rather than pretending it cannot happen.
         if (!PersistentMessageRecipient::DeleteObject(recipient))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6623, "BounceTracker::ApplyAtDelivery",
               Formatter::Format(_T("{0} was not delivered for message {1} because it is on the suppression list, but its recipient row could not be removed from the queue. If the message is tried again for another recipient, this one will be stopped - and, with the action refuse, reported to the sender - again."),
                  address, message->GetID()));
         }

         iterator = recipients.erase(iterator);

         MessageTrace::Record(message->GetID(), MessageTrace::EventSuppressed, sender, address, sendersIP, 550,
            action == ActionDrop ? String(_T("drop: on the suppression list, not delivered")) : String(_T("refuse: on the suppression list, returned to the sender")));

         if (action == ActionDrop)
         {
            LOG_APPLICATION(Formatter::Format(_T("SMTPDeliverer - Message {0}: {1} (BounceSuppressionAction is drop, so it was removed without a report.)"),
               message->GetID(), description));
            continue;
         }

         LOG_APPLICATION(Formatter::Format(_T("SMTPDeliverer - Message {0}: {1} It is returned to the sender."), message->GetID(), description));

         AWStats::LogDeliveryFailure(sendersIP, sender, address, 550, message->GetID());
         Events::FireOnDeliveryFailed(message, sendersIP, address, description);

         if (WantsFailureNotice(recipient))
         {
            failures.push_back(DeliveryFailure(address, enhancedCode,
               Formatter::Format(_T("{0}\r\n   Error Type: SMTP\r\n   Error Description: Not sent: the address is on the suppression list\r\n   Additional information: {1}\r\n\r\n"),
                  address, description)));
         }
      }
   }

   BounceTracker::AddResult
   BounceTracker::AddManual(__int64 domainID, const String &address, const String &note, int expiresDays)
   {
      const String canonical = PersistentBounce::Canonical(address);

      if (canonical.IsEmpty() || !StringParser::IsValidEmailAddress(canonical, true) || address.GetLength() > 255)
         return AddInvalidAddress;

      if (MessageOrigin::IsHostedAddress(canonical))
         return AddHostedAddress;

      if (expiresDays < 0 || expiresDays > 3650)
         return AddInvalidExpiry;

      BounceSuppressionEntry existing;
      const int found = PersistentBounce::FindSuppression(domainID, canonical, existing);

      if (found < 0)
         return AddFailed;

      if (found == 1)
         return AddAlreadyListed;

      BounceSuppressionEntry entry;
      entry.domain_id = domainID;
      entry.address = canonical;
      entry.source = PersistentBounce::SourceManual;
      entry.category = BounceNone;
      entry.status = _T("");
      entry.diagnostic = note;
      entry.created = PersistentBounce::DateTimeFromNow(0);
      entry.expires = expiresDays > 0 ? PersistentBounce::DateTimeFromNow(expiresDays) : String();

      if (!PersistentBounce::InsertSuppression(entry))
         return PersistentBounce::FindSuppression(domainID, canonical, existing) == 1 ? AddAlreadyListed : AddFailed;

      return AddAdded;
   }

   int
   BounceTracker::Remove(__int64 domainID, const String &address)
   {
      // Under the counting lock, so a bounce being counted for the same address at
      // the same moment cannot write its old run back over the reset.
      boost::lock_guard<boost::mutex> guard(CountLockFor(address));

      const int removed = PersistentBounce::DeleteSuppression(domainID, address);

      if (removed <= 0)
         return removed;

      // Removed by hand: the address starts again, and one more hard bounce is one,
      // not the threshold plus one - including one that arrives as a report about a
      // message accepted before the removal, whose register row would otherwise give
      // back the run as it stood then.
      const bool reset = PersistentBounce::ResetConsecutiveHard(domainID, address);
      const bool cleared = PersistentBounce::ClearReportKeyRuns(domainID, PersistentBounce::AddressKey(address));

      if (!reset || !cleared)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6625, "BounceTracker::Remove",
            Formatter::Format(_T("{0} was taken off the suppression list of domain {1}, but its run of hard bounces could not be started again, so it may be listed again sooner than the threshold says."),
               PersistentBounce::Canonical(address), domainID));
      }

      return 1;
   }

   bool
   BounceTracker::RunMaintenance(int &suppressionsExpired, int &countsPruned, int &reportKeysPruned)
   {
      Configuration *configuration = Configuration::Instance();

      suppressionsExpired = PersistentBounce::ExpireSuppressions(configuration->GetBounceSuppressionExpiryDays());
      countsPruned = PersistentBounce::PruneCounts(configuration->GetBounceCountRetentionDays());
      reportKeysPruned = PersistentBounce::PruneReportKeys(configuration->GetBounceReportWindowDays());

      const bool succeeded = suppressionsExpired >= 0 && countsPruned >= 0 && reportKeysPruned >= 0;

      if (suppressionsExpired < 0) suppressionsExpired = 0;
      if (countsPruned < 0) countsPruned = 0;
      if (reportKeysPruned < 0) reportKeysPruned = 0;

      return succeeded;
   }

   bool
   BounceTracker::SuppressionsJson(__int64 domainID, const String &search, int limit, int offset, AnsiString &json)
   {
      std::vector<BounceSuppressionEntry> rows;
      int total = 0;

      if (!PersistentBounce::ListSuppressions(domainID, search, limit, offset, rows, total))
         return false;

      Configuration *configuration = Configuration::Instance();
      const int expiryDays = configuration->GetBounceSuppressionExpiryDays();
      const String now = PersistentBounce::DateTimeFromNow(0);

      // Two flags and not one: processing_enabled is whether bounces are read and
      // counted, list_applies is whether the entries below are enforced. They came
      // apart when the feedback loops began filling this same list - a complaint's
      // entry has to be applied or the feature does nothing - so a page that
      // showed only the first would tell an operator with bounce processing off
      // that nothing is refused while the server refuses.
      json = "{" + JsonField("domain", DomainName(domainID)) + "," +
             JsonBool("processing_enabled", configuration->GetBounceProcessingEnabled()) + "," +
             JsonBool("list_applies", ListApplies_()) + "," +
             JsonField("action", String(ActionName(configuration->GetBounceSuppressionAction()))) + "," +
             JsonNumber("total", total) + "," + JsonNumber("offset", offset < 0 ? 0 : offset) + ",\"items\":[";

      for (size_t i = 0; i < rows.size(); i++)
      {
         const BounceSuppressionEntry &entry = rows[i];

         if (i > 0)
            json += ",";

         json += "{" + JsonField("address", entry.address) + "," +
                 JsonField("source", String(PersistentBounce::SourceName(entry.source))) + "," +
                 JsonField("category", String(BounceClassifier::Name((BounceCategory) entry.category))) + "," +
                 JsonField("status", entry.status) + "," +
                 JsonField("diagnostic", entry.diagnostic) + "," +
                 JsonField("remote_mta", entry.remote_mta) + "," +
                 JsonField("created", entry.created) + "," +
                 JsonField("expires", entry.expires) + "," +
                 JsonBool("expired", PersistentBounce::IsExpired(entry, expiryDays, now)) + "}";
      }

      json += "]}";
      return true;
   }

   AnsiString
   BounceTracker::SuppressionItemJson(__int64 domainID, const String &address)
   {
      BounceSuppressionEntry entry;
      if (PersistentBounce::FindSuppression(domainID, address, entry) != 1)
         return "{}";

      const String now = PersistentBounce::DateTimeFromNow(0);

      return "{" + JsonField("address", entry.address) + "," +
             JsonField("source", String(PersistentBounce::SourceName(entry.source))) + "," +
             JsonField("category", String(BounceClassifier::Name((BounceCategory) entry.category))) + "," +
             JsonField("status", entry.status) + "," +
             JsonField("diagnostic", entry.diagnostic) + "," +
             JsonField("remote_mta", entry.remote_mta) + "," +
             JsonField("created", entry.created) + "," +
             JsonField("expires", entry.expires) + "," +
             JsonBool("expired", PersistentBounce::IsExpired(entry, Configuration::Instance()->GetBounceSuppressionExpiryDays(), now)) + "}";
   }

   bool
   BounceTracker::CountsJson(__int64 domainID, const String &search, int limit, int offset, AnsiString &json)
   {
      std::vector<BounceCountRow> rows;
      int total = 0;

      if (!PersistentBounce::ListCounts(domainID, search, limit, offset, rows, total))
         return false;

      json = "{" + JsonField("domain", DomainName(domainID)) + "," +
             JsonNumber("total", total) + "," + JsonNumber("offset", offset < 0 ? 0 : offset) + ",\"items\":[";

      for (size_t i = 0; i < rows.size(); i++)
      {
         const BounceCountRow &row = rows[i];

         BounceSuppressionEntry entry;
         const bool suppressed = PersistentBounce::FindSuppression(domainID, row.address, entry) == 1;

         if (i > 0)
            json += ",";

         json += "{" + JsonField("address", row.address) + "," +
                 JsonNumber("hard", row.hard) + "," +
                 JsonNumber("soft", row.soft) + "," +
                 JsonNumber("mailbox_full", row.mailbox_full) + "," +
                 JsonNumber("policy", row.policy) + "," +
                 JsonNumber("other", row.other) + "," +
                 JsonNumber("consecutive_hard", row.consecutive_hard) + "," +
                 JsonNumber("complaints", row.complaints) + "," +
                 JsonField("last_status", row.last_status) + "," +
                 JsonField("last_category", String(BounceClassifier::Name((BounceCategory) row.last_category))) + "," +
                 JsonField("last_bounce", row.last_bounce) + "," +
                 JsonField("last_delivered", row.last_delivered) + "," +
                 JsonField("last_complaint", row.last_complaint) + "," +
                 JsonBool("suppressed", suppressed) + "}";
      }

      json += "]}";
      return true;
   }

   AnsiString
   BounceTracker::ClassifyJson(const String &status, const String &diagnostic)
   {
      // A bare status reads as a report's Status field; a reply with its three
      // digits reads as a refusal met while delivering, trust rule and all.
      BounceClassifier::Verdict verdict;

      String diagnosticText = diagnostic;
      diagnosticText.Trim();

      if (BounceClassifier::IsEnhancedStatusCode(status))
      {
         verdict = BounceClassifier::ClassifyReport(status, diagnosticText);
      }
      else
      {
         // The status IS the remote's reply. What delivery would have recorded for
         // it is worked out the way SMTPClientConnection works it out at RCPT TO -
         // the remote's own code, or the one its three digits alone support - and
         // the reply is then judged under the trust rule exactly as a refusal is.
         // Handing the reply over as the recorded code, with no reply beside it,
         // read every reply as this server's own reason and so as "other".
         String reply = status;
         if (!diagnosticText.IsEmpty())
            reply += _T(" ") + diagnosticText;

         const int replyCode = BounceClassifier::ReplyCode(reply);
         const String recorded = replyCode > 0
            ? DeliveryFailure::EnhancedStatusFromSmtpReply(replyCode, Utf8(reply), true)
            : String();

         verdict = BounceClassifier::ClassifyRefusal(recorded, reply);
      }

      const Configuration *configuration = Configuration::Instance();
      const bool mailboxFullIsHard = configuration ? Configuration::Instance()->GetBounceMailboxFullIsHard() : false;

      return "{" + JsonField("category", String(BounceClassifier::Name(verdict.category))) + "," +
             JsonBool("suppresses", BounceClassifier::Suppresses(verdict.category, mailboxFullIsHard)) + "," +
             JsonBool("trusted", verdict.trusted) + "," +
             JsonField("status", verdict.status) + "}";
   }

   void
   BounceReportGateTester::Test()
   {
      BounceReportFacts good;
      good.envelope_sender_empty = true;
      good.received_by_this_server = true;

      String reason;
      Expect(BounceTracker::AcceptReport(good, reason), "A report that passed every check was refused.");

      BounceReportFacts withSender = good;
      withSender.envelope_sender_empty = false;
      Expect(!BounceTracker::AcceptReport(withSender, reason), "A message with an envelope sender was read as a report.");

      BounceReportFacts spam = good;
      spam.spam_flagged = true;
      Expect(!BounceTracker::AcceptReport(spam, reason), "A report classified as spam was accepted.");

      BounceReportFacts authenticated = good;
      authenticated.received_authenticated = true;
      Expect(!BounceTracker::AcceptReport(authenticated, reason), "A report submitted over an authenticated session was accepted.");

      BounceReportFacts ownReport = good;
      ownReport.received_by_this_server = false;
      Expect(!BounceTracker::AcceptReport(ownReport, reason), "A report with no Received field of this server's - its own report - was accepted.");

      BounceReportFacts spfFail = good;
      spfFail.auth_results_spf_fail = true;
      Expect(!BounceTracker::AcceptReport(spfFail, reason), "A report whose SPF failed was accepted.");

      BounceReportFacts dmarcFail = good;
      dmarcFail.auth_results_dmarc_fail = true;
      Expect(!BounceTracker::AcceptReport(dmarcFail, reason), "A report whose DMARC failed was accepted.");

      BounceReportFacts batvPlain = good;
      batvPlain.batv_enabled = true;
      Expect(!BounceTracker::AcceptReport(batvPlain, reason), "With BATV on, a report to a plain address was accepted.");

      BounceReportFacts batvSigned = batvPlain;
      batvSigned.recipient_is_verified_prvs = true;
      Expect(BounceTracker::AcceptReport(batvSigned, reason), "With BATV on, a report to a verified prvs address was refused.");

      // The session-level guard.
      BounceClassifier::Verdict hard;
      hard.category = BounceHard;
      hard.trusted = true;
      hard.status = _T("5.1.1");
      Expect(BounceTracker::ApplySessionGuard(hard, 1).category == BounceHard, "A refusal of one recipient was guarded as a session refusal.");
      Expect(BounceTracker::ApplySessionGuard(hard, 3).category == BounceOther, "An identical refusal of three recipients was counted as three dead mailboxes.");

      BounceClassifier::Verdict policy;
      policy.category = BounceHard;
      policy.status = _T("5.7.1");
      Expect(BounceTracker::ApplySessionGuard(policy, 2).category == BouncePolicy, "A repeated X.7 refusal did not become policy.");
   }

   void
   BounceKeyTester::Test()
   {
      const String key = BounceTracker::ReportKey(_T("<Abc@Example.test>"), _T("Gone@Remote.test"), _T("Sender@Example.test"));

      Expect(key.GetLength() == 64, "The report key is not a SHA-256 in hex.");
      Expect(key == BounceTracker::ReportKey(_T("abc@example.test"), _T("gone@remote.test"), _T("sender@example.test")),
         "The report key depends on case or on the Message-ID's brackets.");
      Expect(key != BounceTracker::ReportKey(_T("abc@example.test"), _T("other@remote.test"), _T("sender@example.test")),
         "A different recipient gave the same report key.");
      Expect(key != BounceTracker::ReportKey(_T("abc@example.test"), _T("gone@remote.test"), _T("someone@example.test")),
         "A different envelope sender gave the same report key.");
      Expect(key != BounceTracker::ReportKey(_T("abd@example.test"), _T("gone@remote.test"), _T("sender@example.test")),
         "A different Message-ID gave the same report key.");

      Expect(PersistentBounce::AddressKey(_T(" Gone@Remote.test ")) == PersistentBounce::AddressKey(_T("gone@remote.test")) &&
             PersistentBounce::AddressKey(_T("gone@remote.test")).GetLength() == 64,
         "The register's address key is not a SHA-256 of the canonical address.");

      // The run a report gives back. Sent and delivered at the same moment is this
      // message's own acceptance.
      const String sent = _T("2026-09-15 10:00:00");

      Expect(BounceTracker::ReportRunIncrease(1, 2, 0, sent, sent) == 3,
         "A hard report for the latest acceptance did not give back the run it set aside, plus one.");
      Expect(BounceTracker::ReportRunIncrease(1, 0, 0, String(), sent) == 1,
         "A hard report for an address with no count row did not start a run of one.");
      Expect(BounceTracker::ReportRunIncrease(0, 2, 0, sent, sent) == 2,
         "A report that does not count towards the list did not give back what its acceptance set aside.");
      Expect(BounceTracker::ReportRunIncrease(1, 2, 1, sent, sent) == 0,
         "A report was added to the run although a later message to the address was accepted and not reported.");
      Expect(BounceTracker::ReportRunIncrease(1, 2, -1, sent, sent) == 0,
         "A report was added to the run although whether a later message was accepted could not be read.");
      Expect(BounceTracker::ReportRunIncrease(1, 2, 0, _T("2026-09-15 10:00:01"), sent) == 0,
         "A report was added to the run although a delivery the register does not hold came after it.");
      Expect(BounceTracker::ReportRunIncrease(1, 2, 0, _T("2026-09-15 09:59:59"), sent) == 3,
         "A delivery before this message was sent stopped its report giving back the run.");

      // Which of a report's recipients are looked up: failed ones, once each, capped.
      DeliveryReport report;
      const wchar_t *actions[] = { L"failed", L"delayed", L"failed", L"failed", L"delivered", L"failed" };
      const wchar_t *addresses[] = { L"a@remote.test", L"b@remote.test", L"A@Remote.test", L"c@remote.test", L"d@remote.test", L"e@remote.test" };
      for (int i = 0; i < 6; i++)
      {
         DeliveryReportRecipient recipient;
         recipient.action = actions[i];
         recipient.final_recipient = addresses[i];
         report.recipients.push_back(recipient);
      }

      bool truncated = true;
      std::vector<size_t> looked = BounceTracker::ReportedFailures(report, 10, truncated);
      Expect(looked.size() == 3 && looked[0] == 0 && looked[1] == 3 && looked[2] == 5 && !truncated,
         "A report's failed recipients were not looked up once each, skipping the ones that did not fail.");

      looked = BounceTracker::ReportedFailures(report, 2, truncated);
      Expect(looked.size() == 2 && truncated, "A report's lookups were not capped, or the cap was not reported.");
   }
}
