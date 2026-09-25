// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "CompromiseDetector.h"
#include "AccountLockout.h"
#include "AlertManager.h"
#include "DiscoveryHolds.h"
#include "MessageUtilities.h"
#include "WebhookManager.h"
#include "RestApiServer.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../Application/ErrorManager.h"
#include "../BO/Account.h"
#include "../BO/Message.h"
#include "../BO/MessageRecipient.h"
#include "../BO/MessageRecipients.h"
#include "../Cache/CacheContainer.h"
#include "../Mime/Mime.h"
#include "../Persistence/PersistentMessage.h"
#include "../Persistence/MessageStore.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../TCPIP/IPAddress.h"
#include "../Util/Parsing/StringParser.h"
#include "../../SMTP/DeliveryQueue.h"

#include <ctime>
#include <map>
#include <set>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A message to more distinct addresses than this has the rest counted
      // as addresses never written to, rather than looked up one by one: it is
      // the shape this exists to catch, and a lookup per address is what a
      // thousand-recipient message would otherwise cost the submission thread.
      const size_t RecipientLookupCap = 100;

      String Clip(const String &text, int length)
      {
         String clipped = text;
         for (int i = 0; i < clipped.GetLength(); i++)
         {
            if (clipped[i] < 0x20)
               clipped.SetAt(i, _T(' '));
         }
         clipped.Trim();
         return clipped.GetLength() > length ? clipped.Mid(0, length) : clipped;
      }

      String Folded(const String &address)
      {
         String folded = address;
         folded.Trim();
         folded.ToLower();
         return folded;
      }

      bool Execute(const SQLCommand &command)
      {
         return Application::Instance()->GetDBManager()->Execute(command);
      }

      std::shared_ptr<DALRecordset> Open(const SQLCommand &command)
      {
         return Application::Instance()->GetDBManager()->OpenRecordset(command);
      }

      // The count a "select count(*) as c ..." answers, or -1 when the
      // database did not answer.
      int Count(const SQLCommand &command)
      {
         std::shared_ptr<DALRecordset> recordset = Open(command);
         if (!recordset || recordset->IsEOF())
            return -1;
         return (int) recordset->GetLongValue("c");
      }

      __int64 BaselineSince(const CompromiseDetector::Thresholds &thresholds, __int64 now)
      {
         return now - (__int64) thresholds.baseline_days * 24 * 60 * 60;
      }

      void Report(int code, const char *where, const String &text)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, code, where, text);
      }

      String HourText(int hour)
      {
         String text;
         text.Format(_T("%02d:00 UTC"), hour);
         return text;
      }

      String WordFor(int sensitivity)
      {
         if (sensitivity == CompromiseDetector::SensitivityRelaxed)
            return _T("relaxed");
         if (sensitivity == CompromiseDetector::SensitivityStrict)
            return _T("strict");
         return _T("default");
      }
   }

   int
   CompromiseDetector::Mode()
   {
      const int mode = Configuration::Instance()->GetCompromiseDetectionMode();
      return mode < ModeOff ? ModeOff : (mode > ModeEnforce ? ModeEnforce : mode);
   }

   CompromiseDetector::Thresholds
   CompromiseDetector::ReadThresholds()
   {
      Configuration *configuration = Configuration::Instance();
      Thresholds thresholds;
      thresholds.baseline_days = configuration->GetCompromiseBaselineDays();
      thresholds.minimum_history = configuration->GetCompromiseMinimumHistory();
      thresholds.volume_factor = configuration->GetCompromiseVolumeFactor();
      thresholds.volume_floor = configuration->GetCompromiseVolumeFloor();
      thresholds.recipient_floor = configuration->GetCompromiseRecipientFloor();
      thresholds.new_recipient_share = configuration->GetCompromiseNewRecipientShare();
      thresholds.score_to_lock = configuration->GetCompromiseScoreToLock();

      if (thresholds.baseline_days < 7) thresholds.baseline_days = 7;
      if (thresholds.baseline_days > 365) thresholds.baseline_days = 365;
      if (thresholds.minimum_history < 1) thresholds.minimum_history = 1;
      if (thresholds.volume_factor < 2) thresholds.volume_factor = 2;
      if (thresholds.volume_floor < 1) thresholds.volume_floor = 1;
      if (thresholds.recipient_floor < 1) thresholds.recipient_floor = 1;
      if (thresholds.new_recipient_share < 1) thresholds.new_recipient_share = 1;
      if (thresholds.new_recipient_share > 100) thresholds.new_recipient_share = 100;
      if (thresholds.score_to_lock < 1) thresholds.score_to_lock = 1;
      return thresholds;
   }

   String
   CompromiseDetector::NetworkOf(const String &address)
   {
      IPAddress parsed;
      if (!parsed.TryParse(AnsiString(address), false))
         return Folded(address);

      const String text = String(parsed.ToString());
      if (parsed.GetType() == IPAddress::IPV4)
      {
         // The first three octets: a client behind one NAT, or on one office
         // or home network, moves inside them.
         const int last = text.ReverseFind(_T('.'));
         return last > 0 ? text.Mid(0, last) + _T(".0/24") : text;
      }

      // The first four groups of the expanded address, which is the /64 a
      // provider hands out, whatever a household's devices do within it.
      std::vector<String> groups = StringParser::SplitString(text, _T(":"));
      String network;
      int taken = 0;
      for (size_t i = 0; i < groups.size() && taken < 4; i++)
      {
         if (groups[i].IsEmpty())
            break;
         network += (taken ? _T(":") : _T("")) + groups[i];
         taken++;
      }
      return taken > 0 ? network + _T("::/64") : text;
   }

   CompromiseDetector::Verdict
   CompromiseDetector::Score(const Inputs &inputs, const Thresholds &thresholds)
   {
      Verdict verdict;
      verdict.has_baseline = inputs.history >= thresholds.minimum_history;

      std::vector<String> signals;
      std::vector<String> reasons;

      // Volume: with a baseline, a surge past the busiest hour on record; without
      // one, the floor alone, which is all a fresh account can be judged by.
      if (inputs.last_hour >= thresholds.volume_floor &&
          (!verdict.has_baseline || inputs.last_hour > (__int64) thresholds.volume_factor * inputs.peak_hour))
      {
         verdict.score += 2;
         signals.push_back(_T("volume"));
         String reason;
         if (verdict.has_baseline)
            reason.Format(_T("%d messages in the last hour against a busiest hour of %d in %d days"), inputs.last_hour, inputs.peak_hour, thresholds.baseline_days);
         else
            reason.Format(_T("%d messages in the last hour from an account with no baseline yet"), inputs.last_hour);
         reasons.push_back(reason);
      }

      // Recipients: enough of them in the hour, and most of them strangers.
      if (inputs.hour_recipients >= thresholds.recipient_floor && inputs.hour_recipients > 0 &&
          (__int64) inputs.hour_new_recipients * 100 >= (__int64) thresholds.new_recipient_share * inputs.hour_recipients)
      {
         verdict.score += 2;
         signals.push_back(_T("recipients"));
         String reason;
         reason.Format(_T("%d of the %d recipients in the last hour are addresses the account has never written to"), inputs.hour_new_recipients, inputs.hour_recipients);
         reasons.push_back(reason);
      }

      // The two that need history to mean anything.
      if (verdict.has_baseline && !inputs.hour_seen)
      {
         verdict.score += 1;
         signals.push_back(_T("hour"));
         String reason;
         reason.Format(_T("nothing sent at this hour of the day in %d days"), thresholds.baseline_days);
         reasons.push_back(reason);
      }

      if (verdict.has_baseline && !inputs.address_seen)
      {
         verdict.score += 1;
         signals.push_back(_T("address"));
         String reason;
         reason.Format(_T("a client network not seen in %d days"), thresholds.baseline_days);
         reasons.push_back(reason);
      }

      int lockAt = thresholds.score_to_lock;
      if (inputs.sensitivity == SensitivityRelaxed)
         lockAt++;
      else if (inputs.sensitivity == SensitivityStrict)
         lockAt--;
      if (lockAt < 1)
         lockAt = 1;

      verdict.takeover = verdict.score >= lockAt;

      for (size_t i = 0; i < signals.size(); i++)
         verdict.signals += (i ? _T(", ") : _T("")) + signals[i];
      for (size_t i = 0; i < reasons.size(); i++)
         verdict.reason += (i ? _T("; ") : _T("")) + reasons[i];
      verdict.reason = Clip(verdict.reason, 255);

      return verdict;
   }

   bool
   CompromiseDetector::ReadReview(__int64 accountId, Review &review)
   {
      review = Review();
      review.account_id = accountId;

      SQLCommand command("select arvexempt, arvsensitivity, arvlocked, arvlockedat, arvunlockedat, arvalertedat, arvscore, arvsignals, arvreason from hm_accountreview where arvaccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountId);
      std::shared_ptr<DALRecordset> recordset = Open(command);
      if (!recordset)
         return false;
      if (recordset->IsEOF())
         return true;

      review.exempt = recordset->GetLongValue("arvexempt") != 0;
      review.sensitivity = (int) recordset->GetLongValue("arvsensitivity");
      review.locked = recordset->GetLongValue("arvlocked") != 0;
      review.locked_at = recordset->GetInt64Value("arvlockedat");
      review.unlocked_at = recordset->GetInt64Value("arvunlockedat");
      review.alerted_at = recordset->GetInt64Value("arvalertedat");
      review.score = (int) recordset->GetLongValue("arvscore");
      review.signals = recordset->GetStringValue("arvsignals");
      review.reason = recordset->GetStringValue("arvreason");
      return true;
   }

   namespace
   {
      // The row, made if it is missing: the review row is the account's state
      // and there is one or none.
      bool EnsureReviewRow(__int64 accountId)
      {
         SQLCommand exists("select arvid from hm_accountreview where arvaccountid = @ACCOUNTID");
         exists.AddParameter("@ACCOUNTID", accountId);
         std::shared_ptr<DALRecordset> recordset = Open(exists);
         if (!recordset)
            return false;
         if (!recordset->IsEOF())
            return true;

         SQLStatement statement(SQLStatement::STInsert, "hm_accountreview");
         statement.AddColumnInt64("arvaccountid", accountId);
         statement.AddColumn("arvexempt", 0);
         statement.AddColumn("arvsensitivity", 0);
         statement.AddColumn("arvlocked", 0);
         statement.AddColumnInt64("arvlockedat", 0);
         statement.AddColumnInt64("arvunlockedat", 0);
         statement.AddColumnInt64("arvalertedat", 0);
         statement.AddColumn("arvscore", 0);
         statement.AddColumn("arvsignals", _T(""));
         statement.AddColumn("arvreason", _T(""));
         statement.SetIdentityColumn("arvid");
         __int64 made = 0;
         return Application::Instance()->GetDBManager()->Execute(statement, &made);
      }

      // The last shape seen, and with it the lock when the shape locked and
      // the alert time when the shape was said.
      bool WriteShape(__int64 accountId, const CompromiseDetector::Verdict &verdict, bool locked, bool alerted, __int64 now)
      {
         if (!EnsureReviewRow(accountId))
            return false;

         // SQL Server and Compact bind a command's parameters by position, not by
         // name: the values go on in the order the placeholders are written, and
         // one name written twice is two placeholders wanting two values. So the
         // moment is named twice over - the lock time and the alert time apart -
         // and each parameter is added where its placeholder stands, only when
         // the clause that carries it is there. Bound any other way the Compact
         // provider put the epoch into arvscore and the score into arvsignals
         // and refused the statement ("Requested conversion is not supported"),
         // so no shape, no lock and no alert time was ever written down.
         AnsiString sql = "update hm_accountreview set arvscore = @SCORE, arvsignals = @SIGNALS, arvreason = @REASON";
         if (locked)
            sql += ", arvlocked = 1, arvlockedat = @LOCKEDAT";
         if (alerted)
            sql += ", arvalertedat = @ALERTEDAT";
         sql += " where arvaccountid = @ACCOUNTID";

         SQLCommand update(sql);
         update.AddParameter("@SCORE", verdict.score);
         update.AddParameter("@SIGNALS", Clip(verdict.signals, 255));
         update.AddParameter("@REASON", Clip(verdict.reason, 255));
         if (locked)
            update.AddParameter("@LOCKEDAT", now);
         if (alerted)
            update.AddParameter("@ALERTEDAT", now);
         update.AddParameter("@ACCOUNTID", accountId);
         return Execute(update);
      }
   }

   bool
   CompromiseDetector::WriteReviewChoices(__int64 accountId, bool exempt, int sensitivity)
   {
      if (!EnsureReviewRow(accountId))
         return false;

      SQLCommand update("update hm_accountreview set arvexempt = @EXEMPT, arvsensitivity = @SENSITIVITY where arvaccountid = @ACCOUNTID");
      update.AddParameter("@EXEMPT", exempt ? 1 : 0);
      update.AddParameter("@SENSITIVITY", sensitivity);
      update.AddParameter("@ACCOUNTID", accountId);
      return Execute(update);
   }

   void
   CompromiseDetector::Assess(std::shared_ptr<const Account> account, const AnsiString &clientAddress,
                              const std::vector<String> &recipients, int source, Assessment &assessment)
   {
      assessment = Assessment();

      try
      {
         const int mode = Mode();
         if (mode == ModeOff || !account || account->GetID() <= 0)
            return;

         Review review;
         if (!ReadReview(account->GetID(), review))
            return;
         if (review.exempt)
            return;

         const Thresholds thresholds = ReadThresholds();
         const __int64 now = (__int64) time(nullptr);

         assessment.mode = mode;
         assessment.account_id = account->GetID();
         assessment.account = account->GetAddress();
         assessment.address = Clip(String(clientAddress), 64);
         assessment.source = source;
         assessment.bucket = now / 3600;
         assessment.hour = (int) (assessment.bucket % 24);

         // Distinct recipients, folded, in the order given.
         std::set<String> seen;
         for (const String &recipient : recipients)
         {
            const String folded = Folded(recipient);
            if (folded.IsEmpty() || seen.count(folded))
               continue;
            seen.insert(folded);
            assessment.recipients.push_back(folded);
         }

         // An account already locked - the session that completed the shape is
         // still open, and a lock refuses new logons, not the connection that
         // is up - submits nothing more until somebody has looked: every
         // further message is held on the standing lock, not judged again,
         // and nothing is re-locked or re-alerted for it.
         if (review.locked && mode == ModeEnforce)
         {
            assessment.already_locked = true;
            assessment.hold = true;
            assessment.verdict.signals = _T("locked");
            assessment.verdict.reason = _T("submitted while the account was locked for review");
            assessment.assessed = true;
            return;
         }

         Inputs inputs;
         inputs.sensitivity = review.sensitivity;

         const __int64 since = BaselineSince(thresholds, now);

         // The last hour starts an hour ago - or at the last unlock, when that
         // is later: an administrator who let the account back in has looked
         // at what it submitted before, and the surge that was a false
         // positive must not be the same shape again on the next message.
         const __int64 hourAgo = now - 3600;
         assessment.hour_from = review.unlocked_at > hourAgo ? review.unlocked_at : hourAgo;
         assessment.alert_due = review.alerted_at <= 0 || now - review.alerted_at >= 3600;

         // The baseline: how much history, and its busiest earlier hour.
         {
            SQLCommand history("select count(*) as c from hm_accountactivity where acaccountid = @ACCOUNTID and actime > @SINCE");
            history.AddParameter("@ACCOUNTID", assessment.account_id);
            history.AddParameter("@SINCE", since);
            inputs.history = Count(history);
            if (inputs.history < 0)
               return;
         }
         {
            SQLCommand peak("select acbucket, count(*) as c from hm_accountactivity where acaccountid = @ACCOUNTID and actime > @SINCE and acbucket < @BUCKET group by acbucket");
            peak.AddParameter("@ACCOUNTID", assessment.account_id);
            peak.AddParameter("@SINCE", since);
            peak.AddParameter("@BUCKET", assessment.bucket);
            std::shared_ptr<DALRecordset> recordset = Open(peak);
            if (!recordset)
               return;
            while (!recordset->IsEOF())
            {
               const int count = (int) recordset->GetLongValue("c");
               if (count > inputs.peak_hour)
                  inputs.peak_hour = count;
               recordset->MoveNext();
            }
         }

         // The last hour: this submission counted in.
         inputs.last_hour = 1;
         inputs.hour_recipients = (int) assessment.recipients.size();
         {
            SQLCommand lastHour("select acrecipients, acnewrecipients from hm_accountactivity where acaccountid = @ACCOUNTID and actime > @HOURFROM");
            lastHour.AddParameter("@ACCOUNTID", assessment.account_id);
            lastHour.AddParameter("@HOURFROM", assessment.hour_from);
            std::shared_ptr<DALRecordset> recordset = Open(lastHour);
            if (!recordset)
               return;
            while (!recordset->IsEOF())
            {
               inputs.last_hour++;
               inputs.hour_recipients += (int) recordset->GetLongValue("acrecipients");
               inputs.hour_new_recipients += (int) recordset->GetLongValue("acnewrecipients");
               recordset->MoveNext();
            }
         }

         // The hours of the day and the networks the account has been seen at.
         inputs.hour_seen = false;
         inputs.address_seen = false;
         {
            SQLCommand hours("select distinct achour from hm_accountactivity where acaccountid = @ACCOUNTID and actime > @SINCE");
            hours.AddParameter("@ACCOUNTID", assessment.account_id);
            hours.AddParameter("@SINCE", since);
            std::shared_ptr<DALRecordset> recordset = Open(hours);
            if (!recordset)
               return;
            while (!recordset->IsEOF())
            {
               if ((int) recordset->GetLongValue("achour") == assessment.hour)
                  inputs.hour_seen = true;
               recordset->MoveNext();
            }
         }
         {
            const String network = NetworkOf(assessment.address);
            SQLCommand addresses("select distinct acaddress from hm_accountactivity where acaccountid = @ACCOUNTID and actime > @SINCE");
            addresses.AddParameter("@ACCOUNTID", assessment.account_id);
            addresses.AddParameter("@SINCE", since);
            std::shared_ptr<DALRecordset> recordset = Open(addresses);
            if (!recordset)
               return;
            while (!recordset->IsEOF())
            {
               if (NetworkOf(recordset->GetStringValue("acaddress")) == network)
                  inputs.address_seen = true;
               recordset->MoveNext();
            }
            // An address that is not there - a submission through a path that
            // knows no peer - is nothing to judge by, and never a signal.
            if (assessment.address.IsEmpty())
               inputs.address_seen = true;
         }

         // Which of this message's recipients the account has written to.
         for (size_t i = 0; i < assessment.recipients.size(); i++)
         {
            if (i >= RecipientLookupCap)
            {
               assessment.new_recipients.push_back(assessment.recipients[i]);
               continue;
            }
            SQLCommand known("select arcid from hm_accountrecipients where arcaccountid = @ACCOUNTID and arcaddress = @ADDRESS");
            known.AddParameter("@ACCOUNTID", assessment.account_id);
            known.AddParameter("@ADDRESS", assessment.recipients[i]);
            std::shared_ptr<DALRecordset> recordset = Open(known);
            if (!recordset)
               return;
            if (recordset->IsEOF())
               assessment.new_recipients.push_back(assessment.recipients[i]);
         }
         inputs.hour_new_recipients += (int) assessment.new_recipients.size();

         assessment.verdict = Score(inputs, thresholds);
         assessment.hold = mode == ModeEnforce && assessment.verdict.takeover;
         assessment.assessed = true;
      }
      catch (...)
      {
         // Nothing judged: the submission goes on as it always has.
         assessment = Assessment();
      }
   }

   namespace
   {
      bool InsertHeld(__int64 accountId, __int64 messageId, int source, const String &sender,
                      const String &recipients, const String &subject, __int64 size, const String &reason, __int64 now)
      {
         SQLStatement statement(SQLStatement::STInsert, "hm_heldmessages");
         statement.AddColumnInt64("hmaccountid", accountId);
         statement.AddColumnInt64("hmmessageid", messageId);
         statement.AddColumnInt64("hmheld", now);
         statement.AddColumn("hmsource", source);
         statement.AddColumn("hmsender", Clip(sender, 255));
         statement.AddColumn("hmrecipients", Clip(recipients, 2000));
         statement.AddColumn("hmsubject", Clip(subject, 255));
         statement.AddColumnInt64("hmsize", size);
         statement.AddColumn("hmreason", Clip(reason, 255));
         statement.SetIdentityColumn("hmid");
         __int64 made = 0;
         return Application::Instance()->GetDBManager()->Execute(statement, &made) && made > 0;
      }

      // Moves a queued message of the account's, submitted within the hour
      // (from the last unlock, when that is later) and not yet picked up, to
      // HeldForReview, and records it. A message the deliverer has locked is
      // in flight and is left alone: the update names messagelocked = 0, so
      // the race is decided by the database. A message waiting for a
      // sandbox's verdict (messagetype 5, always unlocked) is moved too: this
      // hold takes precedence, and the sandbox's poller puts back only what is
      // still 5, so no verdict can send a locked account's mail. Released
      // from review, it meets its sandbox rows again and is held for them.
      int HoldQueued(const CompromiseDetector::Assessment &assessment, __int64 now)
      {
         SQLCommand candidates("select acid, acmessageid from hm_accountactivity where acaccountid = @ACCOUNTID and actime > @HOURFROM and acmessageid > 0 and acheld = 0");
         candidates.AddParameter("@ACCOUNTID", assessment.account_id);
         candidates.AddParameter("@HOURFROM", assessment.hour_from);
         std::shared_ptr<DALRecordset> recordset = Open(candidates);
         if (!recordset)
            return 0;

         std::vector<std::pair<__int64, __int64> > rows;
         while (!recordset->IsEOF())
         {
            rows.push_back(std::make_pair(recordset->GetInt64Value("acid"), recordset->GetInt64Value("acmessageid")));
            recordset->MoveNext();
         }

         int held = 0;
         for (const auto &row : rows)
         {
            SQLCommand hold("update hm_messages set messagetype = 4 where messageid = @MESSAGEID and messagetype in (1, 5) and messagelocked = 0");
            hold.AddParameter("@MESSAGEID", row.second);
            if (!Execute(hold))
               continue;

            std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
            if (!PersistentMessage::ReadObject(message, row.second) || message->GetState() != Message::HeldForReview)
               continue;

            String recipients;
            // owned by this call: the recipients of a message this loop read from the database
            for (std::shared_ptr<MessageRecipient> recipient : message->GetRecipients()->GetVector())
            {
               if (!recipient)
                  continue;
               const String address = recipient->GetOriginalAddress().IsEmpty() ? recipient->GetAddress() : recipient->GetOriginalAddress();
               recipients += (recipients.IsEmpty() ? _T("") : _T(", ")) + address;
            }

            String subject;
            const String fileName = MessageStore::WorkingFile(message);
            AnsiString header = PersistentMessage::LoadHeader(fileName, false);
            if (!header.IsEmpty())
            {
               MimeHeader mimeHeader;
               mimeHeader.Load(header.c_str(), header.GetLength(), true);
               subject = mimeHeader.GetUnicodeFieldValue("Subject");
            }

            if (InsertHeld(assessment.account_id, row.second, assessment.source, message->GetFromAddress(), recipients, subject,
                           (__int64) message->GetSize(), _T("submitted within the hour of: ") + assessment.verdict.reason, now))
            {
               SQLCommand mark("update hm_accountactivity set acheld = 1 where acid = @ID");
               mark.AddParameter("@ID", row.first);
               Execute(mark);
               held++;
            }
         }
         return held;
      }
   }

   void
   CompromiseDetector::Apply(const Assessment &assessment, __int64 messageId, const String &sender,
                             const String &recipientsText, const String &subject, __int64 size)
   {
      if (!assessment.assessed || assessment.account_id <= 0)
         return;

      try
      {
         const __int64 now = (__int64) time(nullptr);

         // The submission, into the baseline.
         {
            SQLStatement statement(SQLStatement::STInsert, "hm_accountactivity");
            statement.AddColumnInt64("acaccountid", assessment.account_id);
            statement.AddColumnInt64("actime", now);
            statement.AddColumnInt64("acbucket", assessment.bucket);
            statement.AddColumn("achour", assessment.hour);
            statement.AddColumn("acrecipients", (long) assessment.recipients.size());
            statement.AddColumn("acnewrecipients", (long) assessment.new_recipients.size());
            statement.AddColumn("acaddress", assessment.address);
            statement.AddColumn("acsource", assessment.source);
            statement.AddColumnInt64("acmessageid", messageId);
            statement.AddColumn("acheld", assessment.hold ? 1 : 0);
            statement.SetIdentityColumn("acid");
            __int64 made = 0;
            if (!Application::Instance()->GetDBManager()->Execute(statement, &made))
            {
               Report(6810, "CompromiseDetector::Apply",
                  _T("The submission by ") + assessment.account + _T(" could not be recorded in hm_accountactivity; the account's baseline is short by one message."));
            }
         }

         // Held on a standing lock: the row that says so, and nothing else.
         if (assessment.already_locked)
         {
            if (!InsertHeld(assessment.account_id, messageId, assessment.source, sender, recipientsText, subject, size, assessment.verdict.reason, now))
            {
               Report(6811, "CompromiseDetector::Apply",
                  _T("Message ") + StringParser::IntToString(messageId) + _T(" from ") + assessment.account +
                  _T(" was kept out of the delivery queue for review but its review row could not be written; it is listed by the sweep as an orphan and discarded."));
            }
            LOG_APPLICATION("CompromiseDetector: a message from " + assessment.account + " was held on the account's standing lock.");
            return;
         }

         // The recipients, into the memory: the strangers added, the rest touched.
         std::set<String> strangers(assessment.new_recipients.begin(), assessment.new_recipients.end());
         for (size_t i = 0; i < assessment.recipients.size() && i < RecipientLookupCap; i++)
         {
            const String &address = assessment.recipients[i];
            if (strangers.count(address))
            {
               SQLStatement statement(SQLStatement::STInsert, "hm_accountrecipients");
               statement.AddColumnInt64("arcaccountid", assessment.account_id);
               statement.AddColumn("arcaddress", Clip(address, 255));
               statement.AddColumnInt64("arcfirst", now);
               statement.AddColumnInt64("arclast", now);
               statement.AddColumn("arccount", 1);
               statement.SetIdentityColumn("arcid");
               __int64 made = 0;
               Application::Instance()->GetDBManager()->Execute(statement, &made);
            }
            else
            {
               SQLCommand touch("update hm_accountrecipients set arclast = @NOW, arccount = arccount + 1 where arcaccountid = @ACCOUNTID and arcaddress = @ADDRESS");
               touch.AddParameter("@NOW", now);
               touch.AddParameter("@ACCOUNTID", assessment.account_id);
               touch.AddParameter("@ADDRESS", address);
               Execute(touch);
            }
         }

         if (!assessment.verdict.takeover)
            return;

         // A takeover shape. In watch mode it is written down and said, and
         // nothing is done to the account; in enforce mode the row's promise.
         // Said once an hour per account: every message after the shape
         // completes scores again for the rest of the hour, and one alert is
         // what a surge is worth - but a lock is a new state and is always
         // said, whatever the hour.
         const bool enforce = assessment.mode == ModeEnforce;
         const bool alert = enforce || assessment.alert_due;
         WriteShape(assessment.account_id, assessment.verdict, enforce, alert, now);

         int othersHeld = 0;
         if (enforce)
         {
            if (!InsertHeld(assessment.account_id, messageId, assessment.source, sender, recipientsText, subject, size, assessment.verdict.reason, now))
            {
               Report(6811, "CompromiseDetector::Apply",
                  _T("Message ") + StringParser::IntToString(messageId) + _T(" from ") + assessment.account +
                  _T(" was kept out of the delivery queue for review but its review row could not be written; it is listed by the sweep as an orphan and discarded."));
            }
            othersHeld = HoldQueued(assessment, now);

            AccountLockout::Instance()->Hold(assessment.account);
            RestApiServer::RevokeSessionsForAccount(assessment.account_id);

            WebhookManager::Event event;
            event.name = WebhookManager::EventAccountLocked;
            event.account = assessment.account;
            event.domain = StringParser::ExtractDomain(assessment.account);
            event.domain.MakeLower();
            event.reason = _T("a takeover shape: ") + assessment.verdict.signals;
            WebhookManager::Instance()->Raise(event);
         }

         if (!alert)
            return;

         // Two formats, two argument lists: the watch-mode sentence has no count
         // in it, so a single Format call handed the count to its second %s and
         // read the integer 1 as a pointer to a string. /EHa let the catch below
         // swallow the access violation, so watch mode raised no alert at all
         // and said nothing about it.
         String summary;
         if (enforce)
            summary.Format(_T("%s was locked and %d message(s) held for review: %s"),
               assessment.account.c_str(), othersHeld + 1, assessment.verdict.signals.c_str());
         else
            summary.Format(_T("%s sent in the shape of a takeover (watch mode, nothing held or locked): %s"),
               assessment.account.c_str(), assessment.verdict.signals.c_str());

         String detail;
         detail.Format(_T("Account: %s\r\nClient address: %s\r\nSubmitted over: %s\r\nScore: %d\r\nSignals: %s\r\nWhy: %s\r\nMode: %s\r\n%s"),
            assessment.account.c_str(),
            assessment.address.c_str(),
            assessment.source == SourceWebmail ? _T("the webmail") : _T("SMTP submission"),
            assessment.verdict.score,
            assessment.verdict.signals.c_str(),
            assessment.verdict.reason.c_str(),
            enforce ? _T("enforce") : _T("watch"),
            enforce
               ? _T("The account cannot sign in until it is unlocked (POST /api/v1/accounts/{address}/unlock, or Unlock on the Deck's Accounts row). What is held is at GET /api/v1/review and the Deck's Held for review page.")
               : _T("Set compromise_detection_mode to enforce to have this lock the account and hold the mail."));

         AlertManager::Instance()->RaiseOccurrence(AlertManager::ConditionAccountCompromised, ErrorManager::High, summary, detail);
         LOG_APPLICATION("CompromiseDetector: " + summary);
      }
      catch (...)
      {
         // The message is saved and, when it was to be held, held: what
         // failed here is the record of it, never the delivery decision.
      }
   }

   namespace
   {
      CompromiseDetector::Held ReadHeldRow(const std::shared_ptr<DALRecordset> &recordset)
      {
         CompromiseDetector::Held held;
         held.id = recordset->GetInt64Value("hmid");
         held.message_id = recordset->GetInt64Value("hmmessageid");
         held.account_id = recordset->GetInt64Value("hmaccountid");
         held.account = recordset->GetStringValue("accountaddress");
         held.sender = recordset->GetStringValue("hmsender");
         held.recipients = recordset->GetStringValue("hmrecipients");
         held.subject = recordset->GetStringValue("hmsubject");
         held.size = recordset->GetInt64Value("hmsize");
         held.held = recordset->GetInt64Value("hmheld");
         held.source = (int) recordset->GetLongValue("hmsource");
         held.reason = recordset->GetStringValue("hmreason");
         return held;
      }

      const char *HeldSelect = "select h.hmid, h.hmmessageid, h.hmaccountid, h.hmsender, h.hmrecipients, h.hmsubject, h.hmsize, h.hmheld, h.hmsource, h.hmreason, a.accountaddress "
                               "from hm_heldmessages h left join hm_accounts a on a.accountid = h.hmaccountid";

      bool DeleteHeldRow(__int64 id)
      {
         SQLCommand remove("delete from hm_heldmessages where hmid = @ID");
         remove.AddParameter("@ID", id);
         return Execute(remove);
      }
   }

   bool
   CompromiseDetector::ListHeld(std::vector<Held> &held)
   {
      held.clear();
      SQLCommand command(AnsiString(HeldSelect) + " order by h.hmid desc");
      std::shared_ptr<DALRecordset> recordset = Open(command);
      if (!recordset)
         return false;
      while (!recordset->IsEOF())
      {
         held.push_back(ReadHeldRow(recordset));
         recordset->MoveNext();
      }
      return true;
   }

   bool
   CompromiseDetector::ReadHeld(__int64 id, Held &held)
   {
      held = Held();
      SQLCommand command(AnsiString(HeldSelect) + " where h.hmid = @ID");
      command.AddParameter("@ID", id);
      std::shared_ptr<DALRecordset> recordset = Open(command);
      if (!recordset || recordset->IsEOF())
         return false;
      held = ReadHeldRow(recordset);
      return true;
   }

   bool
   CompromiseDetector::Release(__int64 id, String &problem)
   {
      Held held;
      if (!ReadHeld(id, held))
      {
         problem = _T("not found");
         return false;
      }

      // A message the webmail sent gets its \Sent copy now, from the queue
      // file while the message is still held and nobody else's: the send
      // made none, because a copy of a message that was then discarded would
      // say it was sent. Before the queue has it, so the deliverer cannot
      // take the file from under the copy; a copy the account has no \Sent
      // folder or no quota for is not made, as the send would not have made
      // one. A scheduled draft is a webmail send too.
      if (held.source == SourceWebmail && held.account_id > 0)
      {
         std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(held.account_id);
         std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
         if (account && PersistentMessage::ReadObject(message, held.message_id) && message->GetID() > 0 && message->GetState() == Message::HeldForReview)
            MessageUtilities::CopyToDesignatedSent(account, message);
      }

      // The ETRN release's own statement, for the same reason it is written
      // this way: the type back to queued and the next try now, on the row
      // only while it is still held.
      SQLCommand release("update hm_messages set messagetype = 1, messagenexttrytime = '1901-01-01 00:00:00' where messageid = @MESSAGEID and messagetype = 4");
      release.AddParameter("@MESSAGEID", held.message_id);
      if (!Execute(release))
      {
         problem = _T("the message could not be put back in the queue");
         return false;
      }

      SQLCommand check("select messagetype from hm_messages where messageid = @MESSAGEID");
      check.AddParameter("@MESSAGEID", held.message_id);
      std::shared_ptr<DALRecordset> recordset = Open(check);
      if (!recordset || recordset->IsEOF())
      {
         DeleteHeldRow(id);
         problem = _T("the held message is gone");
         return false;
      }

      DeleteHeldRow(id);
      Application::Instance()->SubmitPendingEmail();
      return true;
   }

   bool
   CompromiseDetector::Discard(__int64 id, String &problem)
   {
      Held held;
      if (!ReadHeld(id, held))
      {
         problem = _T("not found");
         return false;
      }

      // The queue file's name, taken while the message's row still says it.
      // The queue's removal deletes the row first and the file last, and
      // answers nothing: a recipients delete or a file delete that failed after
      // the row went would leave the recipients or the file - the message's
      // headers and body - with no row to find them by.
      String file;
      {
         std::shared_ptr<Message> message = std::shared_ptr<Message>(new Message());
         if (PersistentMessage::ReadObject(message, held.message_id) && message->GetID() > 0)
            file = MessageStore::WorkingFile(message);
      }

      // The queue's own removal: read, lock, delete the row and the file.
      DeliveryQueue::Remove(held.message_id);

      SQLCommand check("select messagetype from hm_messages where messageid = @MESSAGEID");
      check.AddParameter("@MESSAGEID", held.message_id);
      std::shared_ptr<DALRecordset> recordset = Open(check);
      if (!recordset)
      {
         problem = _T("whether the message was removed could not be read; it may still be held");
         return false;
      }
      if (!recordset->IsEOF())
      {
         problem = _T("the message could not be removed; it is still held");
         return false;
      }

      // Whatever the removal left once the row was gone: its recipients, and
      // its file. A discard answers success only when neither is left.
      SQLCommand recipients("delete from hm_messagerecipients where recipientmessageid = @MESSAGEID");
      recipients.AddParameter("@MESSAGEID", held.message_id);
      if (!Execute(recipients))
      {
         problem = _T("the message's recipients could not be removed");
         return false;
      }

      if (!file.IsEmpty() && MessageStore::Exists(file) && !MessageStore::DiscardWorkingFile(file))
      {
         problem = _T("the message's file could not be deleted: ") + file;
         return false;
      }

      if (!DeleteHeldRow(id))
      {
         problem = _T("the message is gone but its review row could not be removed");
         return false;
      }

      return true;
   }

   int
   CompromiseDetector::EraseAddress(const String &address, int &held)
   {
      held = 0;

      const String wanted = Folded(address);
      if (wanted.IsEmpty())
         return 0;

      // The held messages whose queued message itself names the address: the
      // envelope sender, or a recipient as the queue will deliver to it or as
      // it was addressed (an alias, a list). The review row's recipients are a
      // display copy - the original addresses, cut at 2,000 characters - and a
      // surge to a few hundred strangers, or an address reached through an
      // alias, is not all in it; a release would deliver to every recipient
      // row, so the rows are what decide.
      std::set<__int64> namedByQueue;
      {
         SQLCommand byRecipient("select h.hmid from hm_heldmessages h inner join hm_messagerecipients r on r.recipientmessageid = h.hmmessageid "
                                "where lower(r.recipientaddress) = @ADDRESS or lower(r.recipientoriginaladdress) = @ORIGINAL");
         byRecipient.AddParameter("@ADDRESS", wanted);
         byRecipient.AddParameter("@ORIGINAL", wanted);
         std::shared_ptr<DALRecordset> recordset = Open(byRecipient);
         if (!recordset)
            return -1;
         while (!recordset->IsEOF())
         {
            namedByQueue.insert(recordset->GetInt64Value("hmid"));
            recordset->MoveNext();
         }
      }
      {
         SQLCommand bySender("select h.hmid from hm_heldmessages h inner join hm_messages m on m.messageid = h.hmmessageid "
                             "where lower(m.messagefrom) = @SENDER");
         bySender.AddParameter("@SENDER", wanted);
         std::shared_ptr<DALRecordset> recordset = Open(bySender);
         if (!recordset)
            return -1;
         while (!recordset->IsEOF())
         {
            namedByQueue.insert(recordset->GetInt64Value("hmid"));
            recordset->MoveNext();
         }
      }

      // Every held message, as the review page lists them: they are few - a
      // takeover's surge, reviewed or discarded within CompromiseHeldDays. The
      // review row's own sender and recipients are matched too, whole and in
      // memory as the quarantine's are, for a row whose message has already
      // left the queue.
      std::vector<Held> entries;
      if (!ListHeld(entries))
         return -1;

      int discarded = 0;
      bool failed = false;

      for (const Held &entry : entries)
      {
         bool names = namedByQueue.count(entry.id) > 0 || Folded(entry.sender) == wanted;
         if (!names)
         {
            for (const String &recipient : StringParser::SplitString(entry.recipients, _T(",")))
            {
               if (Folded(recipient) == wanted)
               {
                  names = true;
                  break;
               }
            }
         }

         if (!names)
            continue;

         // A legal hold outranks the erasure, as it outranks every other
         // deletion of the account's mail: what the account submitted stays for
         // the review, and the case.
         if (DiscoveryHolds::IsAccountHeld(entry.account_id))
         {
            held++;
            continue;
         }

         String problem;
         if (Discard(entry.id, problem))
         {
            discarded++;
         }
         else
         {
            // Named by its id alone: the account that sent it may be the very
            // address being erased, whose log lines the erasure may be about
            // to rewrite.
            failed = true;
            Report(7380, "CompromiseDetector::EraseAddress",
               _T("The message ") + StringParser::IntToString(entry.message_id) + _T(" held for review (review row ") + StringParser::IntToString(entry.id) +
               _T(") names an address being erased but could not be discarded, and the erasure fails until it is: ") + problem);
         }
      }

      if (discarded > 0)
         LOG_APPLICATION("CompromiseDetector: " + StringParser::IntToString(discarded) + " message(s) held for review were discarded by an address's erasure.");

      return failed ? -1 : discarded;
   }

   bool
   CompromiseDetector::ReadBaseline(__int64 accountId, Baseline &baseline)
   {
      baseline = Baseline();
      const Thresholds thresholds = ReadThresholds();
      const __int64 now = (__int64) time(nullptr);
      const __int64 since = BaselineSince(thresholds, now);
      baseline.days = thresholds.baseline_days;
      baseline.minimum_history = thresholds.minimum_history;

      {
         SQLCommand history("select count(*) as c from hm_accountactivity where acaccountid = @ACCOUNTID and actime > @SINCE");
         history.AddParameter("@ACCOUNTID", accountId);
         history.AddParameter("@SINCE", since);
         baseline.history = Count(history);
         if (baseline.history < 0)
            return false;
      }
      {
         SQLCommand lastHour("select count(*) as c from hm_accountactivity where acaccountid = @ACCOUNTID and actime > @HOURAGO");
         lastHour.AddParameter("@ACCOUNTID", accountId);
         lastHour.AddParameter("@HOURAGO", now - 3600);
         baseline.last_hour = Count(lastHour);
         if (baseline.last_hour < 0)
            return false;
      }
      {
         SQLCommand peak("select acbucket, count(*) as c from hm_accountactivity where acaccountid = @ACCOUNTID and actime > @SINCE group by acbucket");
         peak.AddParameter("@ACCOUNTID", accountId);
         peak.AddParameter("@SINCE", since);
         std::shared_ptr<DALRecordset> recordset = Open(peak);
         if (!recordset)
            return false;
         while (!recordset->IsEOF())
         {
            const int count = (int) recordset->GetLongValue("c");
            if (count > baseline.peak_hour)
               baseline.peak_hour = count;
            recordset->MoveNext();
         }
      }
      {
         SQLCommand hours("select distinct achour from hm_accountactivity where acaccountid = @ACCOUNTID and actime > @SINCE order by achour");
         hours.AddParameter("@ACCOUNTID", accountId);
         hours.AddParameter("@SINCE", since);
         std::shared_ptr<DALRecordset> recordset = Open(hours);
         if (!recordset)
            return false;
         while (!recordset->IsEOF())
         {
            baseline.hours.push_back((int) recordset->GetLongValue("achour"));
            recordset->MoveNext();
         }
      }
      {
         SQLCommand addresses("select distinct acaddress from hm_accountactivity where acaccountid = @ACCOUNTID and actime > @SINCE");
         addresses.AddParameter("@ACCOUNTID", accountId);
         addresses.AddParameter("@SINCE", since);
         std::shared_ptr<DALRecordset> recordset = Open(addresses);
         if (!recordset)
            return false;
         std::set<String> networks;
         while (!recordset->IsEOF())
         {
            networks.insert(NetworkOf(recordset->GetStringValue("acaddress")));
            recordset->MoveNext();
         }
         baseline.networks.assign(networks.begin(), networks.end());
      }
      {
         SQLCommand known("select count(*) as c from hm_accountrecipients where arcaccountid = @ACCOUNTID");
         known.AddParameter("@ACCOUNTID", accountId);
         baseline.recipients_known = Count(known);
         if (baseline.recipients_known < 0)
            return false;
      }
      {
         SQLCommand held("select count(*) as c from hm_heldmessages where hmaccountid = @ACCOUNTID");
         held.AddParameter("@ACCOUNTID", accountId);
         baseline.held = Count(held);
         if (baseline.held < 0)
            return false;
      }
      return true;
   }

   void
   CompromiseDetector::Unlock(__int64 accountId, const String &address)
   {
      try
      {
         // The time is the fresh start Assess counts from: what was
         // submitted before it has been looked at.
         SQLCommand update("update hm_accountreview set arvlocked = 0, arvunlockedat = @NOW where arvaccountid = @ACCOUNTID and arvlocked = 1");
         update.AddParameter("@NOW", (__int64) time(nullptr));
         update.AddParameter("@ACCOUNTID", accountId);
         Execute(update);
         AccountLockout::Instance()->Release(address);
      }
      catch (...)
      {
      }
   }

   void
   CompromiseDetector::ForgetAccount(__int64 accountId, const String &address)
   {
      try
      {
         SQLCommand command("select hmid, hmmessageid from hm_heldmessages where hmaccountid = @ACCOUNTID");
         command.AddParameter("@ACCOUNTID", accountId);
         std::shared_ptr<DALRecordset> recordset = Open(command);
         std::vector<std::pair<__int64, __int64> > rows;
         while (recordset && !recordset->IsEOF())
         {
            rows.push_back(std::make_pair(recordset->GetInt64Value("hmid"), recordset->GetInt64Value("hmmessageid")));
            recordset->MoveNext();
         }
         for (const auto &row : rows)
         {
            DeliveryQueue::Remove(row.second);
            DeleteHeldRow(row.first);
         }
         if (!rows.empty())
            LOG_APPLICATION("CompromiseDetector: " + StringParser::IntToString((int) rows.size()) + " message(s) held for review from " + address + " were discarded with the account.");

         AccountLockout::Instance()->Release(address);
      }
      catch (...)
      {
      }
   }

   void
   CompromiseDetector::LoadLocks()
   {
      try
      {
         SQLCommand command("select a.accountaddress from hm_accountreview r inner join hm_accounts a on a.accountid = r.arvaccountid where r.arvlocked = 1");
         std::shared_ptr<DALRecordset> recordset = Open(command);
         if (!recordset)
         {
            Report(6813, "CompromiseDetector::LoadLocks",
               _T("The compromise locks could not be read from hm_accountreview; a name locked before this start is not held until the next sweep reads them."));
            return;
         }

         std::vector<String> names;
         while (!recordset->IsEOF())
         {
            names.push_back(recordset->GetStringValue("accountaddress"));
            recordset->MoveNext();
         }
         AccountLockout::Instance()->SetHeld(names);
      }
      catch (...)
      {
      }
   }

   int
   CompromiseDetector::Sweep()
   {
      int discarded = 0;
      try
      {
         const Thresholds thresholds = ReadThresholds();
         const __int64 now = (__int64) time(nullptr);
         const __int64 since = BaselineSince(thresholds, now);

         SQLCommand activity("delete from hm_accountactivity where actime <= @SINCE");
         activity.AddParameter("@SINCE", since);
         Execute(activity);

         SQLCommand recipients("delete from hm_accountrecipients where arclast <= @SINCE");
         recipients.AddParameter("@SINCE", since);
         Execute(recipients);

         // Held messages nobody reviewed within the window: discarded, and said.
         int heldDays = Configuration::Instance()->GetCompromiseHeldDays();
         if (heldDays < 1) heldDays = 1;
         if (heldDays > 90) heldDays = 90;
         std::vector<Held> held;
         if (ListHeld(held))
         {
            for (const Held &entry : held)
            {
               if (entry.held > now - (__int64) heldDays * 24 * 60 * 60)
                  continue;
               String problem;
               if (Discard(entry.id, problem))
               {
                  discarded++;
                  LOG_APPLICATION("CompromiseDetector: the message " + StringParser::IntToString(entry.message_id) + " held for review from " + entry.account +
                                  " was not reviewed within " + StringParser::IntToString(heldDays) + " days and was discarded.");
               }
               else
               {
                  Report(6812, "CompromiseDetector::Sweep",
                     _T("The message ") + StringParser::IntToString(entry.message_id) + _T(" held for review from ") + entry.account +
                     _T(" is past ") + StringParser::IntToString(heldDays) + _T(" days but could not be discarded: ") + problem);
               }
            }
         }

         // A hold row whose message is gone, and a held message whose row is
         // gone - the account was deleted and the row went with it - are each
         // ended: the second by the queue's own removal, because mail from an
         // account that no longer exists has nobody to release it.
         if (ListHeld(held))
         {
            for (const Held &entry : held)
            {
               SQLCommand exists("select messageid from hm_messages where messageid = @MESSAGEID and messagetype = 4");
               exists.AddParameter("@MESSAGEID", entry.message_id);
               std::shared_ptr<DALRecordset> recordset = Open(exists);
               if (recordset && recordset->IsEOF())
                  DeleteHeldRow(entry.id);
            }
         }
         {
            // A held message younger than ten minutes is left alone whatever
            // its rows say: between the save that made it type 4 and the
            // hold row that names it, the submission thread is still reading
            // the header for the subject, and a sweep in that window would
            // take the message from under a row about to be written. The
            // create time is the local-clock text the save wrote, so the
            // cutoff is made the same way; if it cannot be, nothing is young
            // enough to be sure of and the pass discards nothing.
            String cutoff;
            {
               const time_t cutoffTime = time(nullptr) - 10 * 60;
               struct tm cutoffTm;
               char cutoffText[32];
               if (localtime_s(&cutoffTm, &cutoffTime) == 0 && strftime(cutoffText, sizeof(cutoffText), "%Y-%m-%d %H:%M:%S", &cutoffTm) != 0)
                  cutoff = String(cutoffText);
            }

            SQLCommand orphans("select messageid, messagecreatetime from hm_messages where messagetype = 4");
            std::shared_ptr<DALRecordset> recordset = Open(orphans);
            std::vector<__int64> ids;
            while (recordset && !recordset->IsEOF())
            {
               const String created = recordset->GetStringValue("messagecreatetime");
               if (!cutoff.IsEmpty() && created.GetLength() >= 19 && created.Mid(0, 19) <= cutoff)
                  ids.push_back(recordset->GetInt64Value("messageid"));
               recordset->MoveNext();
            }
            for (__int64 id : ids)
            {
               SQLCommand row("select hmid from hm_heldmessages where hmmessageid = @MESSAGEID");
               row.AddParameter("@MESSAGEID", id);
               std::shared_ptr<DALRecordset> answer = Open(row);
               if (answer && answer->IsEOF())
               {
                  DeliveryQueue::Remove(id);
                  discarded++;
                  LOG_APPLICATION("CompromiseDetector: the held message " + StringParser::IntToString(id) + " belonged to an account that no longer exists and was discarded.");
               }
            }
         }

         LoadLocks();
      }
      catch (...)
      {
      }
      return discarded;
   }
}
