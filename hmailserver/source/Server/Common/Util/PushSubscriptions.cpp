// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "PushSubscriptions.h"

#include "Hashing/HashCreator.h"
#include "WebPush.h"
#include "../Application/Application.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"

#include <stdexcept>
#include <time.h>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The column widths of hm_pushsubscriptions (schema 6050). A value longer
      // than its column is refused here rather than truncated by the backend,
      // because a truncated endpoint is a subscription that can never be found
      // again and a truncated key is one that can never be encrypted to.
      const int EndpointMaximum = 1000;
      const int KeyMaximum = 128;
      const int AuthMaximum = 32;
      const int DeviceMaximum = 64;
      const int ListMaximum = 255;
      const int DetailMaximum = 255;

      const int MinutesInADay = 24 * 60;

      String Int64Text(__int64 value)
      {
         String text;
         text.Format(_T("%I64d"), value);
         return text;
      }

      // "a@b.com, @work.example" -> the trimmed, lower-cased parts.
      void SplitList(const String &list, std::vector<String> &parts)
      {
         parts.clear();

         String current;
         for (int i = 0; i <= list.GetLength(); i++)
         {
            if (i == list.GetLength() || list[i] == ',')
            {
               current.Trim();
               current.MakeLower();
               if (!current.IsEmpty())
                  parts.push_back(current);
               current.Empty();
               continue;
            }

            current += list[i];
         }
      }
   }

   String
   PushSubscriptions::HashOf(const String &endpoint)
   {
      HashCreator hasher(HashCreator::SHA256);
      return String(hasher.GenerateHashNoSalt(AnsiString(endpoint), HashCreator::hex));
   }

   PushSubscriptions::Subscription
   PushSubscriptions::Read_(const std::shared_ptr<DALRecordset> &recordset)
   {
      Subscription subscription;

      subscription.id = recordset->GetInt64Value("psid");
      subscription.account_id = recordset->GetInt64Value("psaccountid");
      subscription.endpoint_hash = recordset->GetStringValue("psendpointhash");
      subscription.endpoint = recordset->GetStringValue("psendpoint");
      subscription.p256dh = recordset->GetStringValue("pskeyp256dh");
      subscription.auth = recordset->GetStringValue("pskeyauth");
      subscription.device = recordset->GetStringValue("psdevice");
      subscription.created = recordset->GetInt64Value("pscreated");
      subscription.last_sent = recordset->GetInt64Value("pslastsent");
      subscription.state = recordset->GetLongValue("psstate");
      subscription.failures = recordset->GetLongValue("psfailures");
      subscription.next_attempt = recordset->GetInt64Value("psnextattempt");
      subscription.last_detail = recordset->GetStringValue("pslastdetail");
      subscription.detail = recordset->GetLongValue("psdetail");
      subscription.senders = recordset->GetStringValue("pssenders");
      subscription.labels = recordset->GetStringValue("pslabels");
      subscription.quiet_from = recordset->GetLongValue("psquietfrom");
      subscription.quiet_to = recordset->GetLongValue("psquietto");
      subscription.quiet_offset = recordset->GetLongValue("psquietoffset");

      return subscription;
   }

   bool
   PushSubscriptions::ForAccount(__int64 accountId, std::vector<Subscription> &subscriptions)
   {
      subscriptions.clear();

      SQLCommand command(_T("select * from hm_pushsubscriptions where psaccountid = @ACCOUNTID order by psid asc"));
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         subscriptions.push_back(Read_(recordset));
         recordset->MoveNext();
      }

      return true;
   }

   bool
   PushSubscriptions::ActiveAccounts(std::vector<__int64> &accountIds)
   {
      accountIds.clear();

      // distinct rather than group by, and no subquery: SQL Server Compact is one
      // of the four backends and is the least forgiving of the two.
      SQLCommand command(_T("select distinct psaccountid from hm_pushsubscriptions where psstate = 1"));

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         accountIds.push_back(recordset->GetInt64Value("psaccountid"));
         recordset->MoveNext();
      }

      return true;
   }

   bool
   PushSubscriptions::ById(__int64 accountId, __int64 id, Subscription &subscription)
   {
      SQLCommand command(_T("select * from hm_pushsubscriptions where psaccountid = @ACCOUNTID and psid = @ID"));
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@ID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      subscription = Read_(recordset);
      return true;
   }

   int
   PushSubscriptions::CountForAccount(__int64 accountId)
   {
      SQLCommand command(_T("select count(*) as subscriptioncount from hm_pushsubscriptions where psaccountid = @ACCOUNTID"));
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return 0;

      return (int) recordset->GetLongValue("subscriptioncount");
   }

   bool
   PushSubscriptions::IsLoopbackHttp_(const String &lowerCaseEndpoint)
   {
      // "http://127.0.0.1:port/...", "http://localhost:port/..." or "http://[::1]:port/...".
      if (!lowerCaseEndpoint.StartsWith(_T("http://")))
         return false;

      String rest = lowerCaseEndpoint.Mid(7);
      return rest.StartsWith(_T("127.0.0.1")) || rest.StartsWith(_T("localhost")) || rest.StartsWith(_T("[::1]"));
   }

   bool
   PushSubscriptions::Save(Subscription &subscription, String &refusal)
   {
      refusal.Empty();

      String endpoint = subscription.endpoint;
      endpoint.Trim();

      if (endpoint.IsEmpty() || endpoint.GetLength() > EndpointMaximum)
      {
         refusal = _T("a push endpoint is between one and a thousand characters");
         return false;
      }

      // An https endpoint, or plain http to a loopback address and nowhere else -
      // which is HttpsClient's own rule, and is what lets the regression suite
      // stand a push service up on 127.0.0.1. A push service in the field is
      // always https, and a plain http one on a network would carry the VAPID
      // token that authorises sending to that device in the clear.
      String lower = endpoint;
      lower.MakeLower();
      if (!lower.StartsWith(_T("https://")) && !IsLoopbackHttp_(lower))
      {
         refusal = _T("a push endpoint is an https address");
         return false;
      }

      // The two keys have to be the lengths RFC 8291 needs, and they are checked
      // by decoding them rather than by counting characters: a key that is not a
      // point is a device that can never be sent to, and finding that out now is
      // the difference between a refusal a person can read and a push that
      // silently never arrives.
      std::vector<unsigned char> point;
      if (!WebPush::Base64UrlDecode(AnsiString(subscription.p256dh), point) || point.size() != 65 || point[0] != 0x04)
      {
         refusal = _T("the p256dh key is not a 65-octet P-256 point in base64url");
         return false;
      }

      std::vector<unsigned char> auth;
      if (!WebPush::Base64UrlDecode(AnsiString(subscription.auth), auth) || auth.size() != 16)
      {
         refusal = _T("the auth secret is not 16 octets in base64url");
         return false;
      }

      if (subscription.p256dh.GetLength() > KeyMaximum || subscription.auth.GetLength() > AuthMaximum)
      {
         refusal = _T("the subscription's keys are longer than the store holds");
         return false;
      }

      String device = subscription.device;
      device.Trim();
      if (device.GetLength() > DeviceMaximum)
         device = device.Mid(0, DeviceMaximum);
      if (device.IsEmpty())
         device = _T("This device");

      if (subscription.detail < DetailCount || subscription.detail > DetailSubject)
      {
         refusal = _T("what a push may carry is 0 (a count), 1 (the sender) or 2 (the sender and the subject)");
         return false;
      }

      if (subscription.senders.GetLength() > ListMaximum || subscription.labels.GetLength() > ListMaximum)
      {
         refusal = _T("the senders and labels a push is for are at most 255 characters each");
         return false;
      }

      if ((subscription.quiet_from < -1 || subscription.quiet_from >= MinutesInADay) ||
          (subscription.quiet_to < -1 || subscription.quiet_to >= MinutesInADay))
      {
         refusal = _T("quiet hours are minutes past midnight, 0 to 1439, or -1 for none");
         return false;
      }

      if (subscription.quiet_offset < -MinutesInADay || subscription.quiet_offset > MinutesInADay)
      {
         refusal = _T("the device's offset from UTC is in minutes, within a day either way");
         return false;
      }

      subscription.endpoint = endpoint;
      subscription.device = device;
      subscription.endpoint_hash = HashOf(endpoint);

      __int64 now = (__int64) time(nullptr);

      // The same device subscribing again - which happens whenever a push service
      // rotates the endpoint - updates its own row. Found by the hash, which is
      // what the unique index is on.
      SQLCommand find(_T("select psid from hm_pushsubscriptions where psaccountid = @ACCOUNTID and psendpointhash = @HASH"));
      find.AddParameter("@ACCOUNTID", subscription.account_id);
      find.AddParameter("@HASH", subscription.endpoint_hash);

      std::shared_ptr<DALRecordset> existing = Application::Instance()->GetDBManager()->OpenRecordset(find);
      __int64 existingId = (existing && !existing->IsEOF()) ? existing->GetInt64Value("psid") : 0;

      SQLStatement statement;
      statement.SetTable(_T("hm_pushsubscriptions"));
      statement.AddColumn(_T("psendpointhash"), subscription.endpoint_hash);
      statement.AddColumn(_T("psendpoint"), subscription.endpoint);
      statement.AddColumn(_T("pskeyp256dh"), subscription.p256dh);
      statement.AddColumn(_T("pskeyauth"), subscription.auth);
      statement.AddColumn(_T("psdevice"), subscription.device);
      statement.AddColumn(_T("psstate"), (long) StateActive);
      statement.AddColumn(_T("psfailures"), (long) 0);
      statement.AddColumnInt64(_T("psnextattempt"), 0);
      statement.AddColumn(_T("pslastdetail"), _T(""));
      statement.AddColumn(_T("psdetail"), (long) subscription.detail);
      statement.AddColumn(_T("pssenders"), subscription.senders);
      statement.AddColumn(_T("pslabels"), subscription.labels);
      statement.AddColumn(_T("psquietfrom"), (long) subscription.quiet_from);
      statement.AddColumn(_T("psquietto"), (long) subscription.quiet_to);
      statement.AddColumn(_T("psquietoffset"), (long) subscription.quiet_offset);

      if (existingId > 0)
      {
         statement.SetStatementType(SQLStatement::STUpdate);
         statement.SetWhereClause(_T("psid = ") + Int64Text(existingId));

         if (!Application::Instance()->GetDBManager()->Execute(statement))
         {
            refusal = _T("the subscription could not be saved");
            return false;
         }

         subscription.id = existingId;
         return true;
      }

      statement.SetStatementType(SQLStatement::STInsert);
      statement.SetIdentityColumn(_T("psid"));
      statement.AddColumnInt64(_T("psaccountid"), subscription.account_id);
      statement.AddColumnInt64(_T("pscreated"), now);
      statement.AddColumnInt64(_T("pslastsent"), 0);

      __int64 id = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id <= 0)
      {
         refusal = _T("the subscription could not be saved");
         return false;
      }

      subscription.id = id;
      subscription.created = now;
      return true;
   }

   bool
   PushSubscriptions::SaveChoice(__int64 accountId, __int64 id, int detail, const String &senders, const String &labels,
                                 int quietFrom, int quietTo, int quietOffset, String &refusal)
   {
      refusal.Empty();

      Subscription existing;
      if (!ById(accountId, id, existing))
      {
         refusal = _T("there is no such device");
         return false;
      }

      if (detail < DetailCount || detail > DetailSubject)
      {
         refusal = _T("what a push may carry is 0 (a count), 1 (the sender) or 2 (the sender and the subject)");
         return false;
      }

      if (senders.GetLength() > ListMaximum || labels.GetLength() > ListMaximum)
      {
         refusal = _T("the senders and labels a push is for are at most 255 characters each");
         return false;
      }

      if ((quietFrom < -1 || quietFrom >= MinutesInADay) || (quietTo < -1 || quietTo >= MinutesInADay))
      {
         refusal = _T("quiet hours are minutes past midnight, 0 to 1439, or -1 for none");
         return false;
      }

      if (quietOffset < -MinutesInADay || quietOffset > MinutesInADay)
      {
         refusal = _T("the device's offset from UTC is in minutes, within a day either way");
         return false;
      }

      SQLStatement statement(SQLStatement::STUpdate, _T("hm_pushsubscriptions"));
      statement.AddColumn(_T("psdetail"), (long) detail);
      statement.AddColumn(_T("pssenders"), senders);
      statement.AddColumn(_T("pslabels"), labels);
      statement.AddColumn(_T("psquietfrom"), (long) quietFrom);
      statement.AddColumn(_T("psquietto"), (long) quietTo);
      statement.AddColumn(_T("psquietoffset"), (long) quietOffset);
      statement.SetWhereClause(_T("psid = ") + Int64Text(id));

      if (!Application::Instance()->GetDBManager()->Execute(statement))
      {
         refusal = _T("the device's settings could not be saved");
         return false;
      }

      return true;
   }

   bool
   PushSubscriptions::Remove(__int64 accountId, __int64 id)
   {
      SQLCommand command(_T("delete from hm_pushsubscriptions where psaccountid = @ACCOUNTID and psid = @ID"));
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@ID", id);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   PushSubscriptions::RemoveByEndpoint(__int64 accountId, const String &endpoint)
   {
      SQLCommand command(_T("delete from hm_pushsubscriptions where psaccountid = @ACCOUNTID and psendpointhash = @HASH"));
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@HASH", HashOf(endpoint));

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   PushSubscriptions::MarkDelivered(__int64 id, __int64 when)
   {
      SQLCommand command(_T("update hm_pushsubscriptions set pslastsent = @WHEN, psfailures = 0, psnextattempt = 0, ")
                         _T("psstate = 1, pslastdetail = @DETAIL where psid = @ID"));
      command.AddParameter("@WHEN", when);
      command.AddParameter("@DETAIL", String(_T("")));
      command.AddParameter("@ID", id);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   PushSubscriptions::MarkFailed(__int64 id, int failures, __int64 nextAttempt, int state, const String &detail)
   {
      String text = detail;
      if (text.GetLength() > DetailMaximum)
         text = text.Mid(0, DetailMaximum);

      SQLCommand command(_T("update hm_pushsubscriptions set psfailures = @FAILURES, psnextattempt = @NEXT, ")
                         _T("psstate = @STATE, pslastdetail = @DETAIL where psid = @ID"));
      command.AddParameter("@FAILURES", failures);
      command.AddParameter("@NEXT", nextAttempt);
      command.AddParameter("@STATE", state);
      command.AddParameter("@DETAIL", text);
      command.AddParameter("@ID", id);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   PushSubscriptions::SenderMatches(const String &list, const String &sender)
   {
      std::vector<String> parts;
      SplitList(list, parts);

      if (parts.empty())
         return true;

      String address = sender;
      address.Trim();
      address.MakeLower();

      if (address.IsEmpty())
         return false;

      for (const String &part : parts)
      {
         if (part.StartsWith(_T("@")))
         {
            if (address.EndsWith(part.c_str()))
               return true;

            continue;
         }

         if (address == part)
            return true;
      }

      return false;
   }

   bool
   PushSubscriptions::LabelMatches(const String &list, const String &keywords)
   {
      std::vector<String> wanted;
      SplitList(list, wanted);

      if (wanted.empty())
         return true;

      // The message's keywords are stored space-separated in hm_messages; a comma
      // is accepted here too so that the same splitter reads both.
      String carried = keywords;
      carried.Replace(_T(" "), _T(","));

      std::vector<String> has;
      SplitList(carried, has);

      for (const String &one : wanted)
      {
         for (const String &other : has)
         {
            if (one == other)
               return true;
         }
      }

      return false;
   }

   bool
   PushSubscriptions::IsQuiet(const Subscription &subscription, __int64 now)
   {
      if (subscription.quiet_from < 0 || subscription.quiet_to < 0)
         return false;

      // A window whose ends are the same is not "all day": it is nothing, which is
      // what a person who set both to the same time meant, and the alternative -
      // silence for ever - is the worse way to be wrong.
      if (subscription.quiet_from == subscription.quiet_to)
         return false;

      __int64 local = now + (__int64) subscription.quiet_offset * 60;

      // The minute of the device's own day. The modulus is taken twice because a
      // negative offset before midnight UTC makes the first one negative.
      int minute = (int) (((local / 60) % MinutesInADay + MinutesInADay) % MinutesInADay);

      if (subscription.quiet_from < subscription.quiet_to)
         return minute >= subscription.quiet_from && minute < subscription.quiet_to;

      // The window wraps midnight: 22:00 to 07:00 is quiet from 22:00 to the end
      // of the day and from the start of the day to 07:00.
      return minute >= subscription.quiet_from || minute < subscription.quiet_to;
   }

   bool
   PushSubscriptions::Wants(const Subscription &subscription, const String &sender, const String &keywords, __int64 now)
   {
      if (subscription.state != StateActive)
         return false;

      if (IsQuiet(subscription, now))
         return false;

      if (!SenderMatches(subscription.senders, sender))
         return false;

      return LabelMatches(subscription.labels, keywords);
   }

   // ------------------------------------------------------------------------
   // The in-process tests, run by ClassTester::DoTests.
   // ------------------------------------------------------------------------

   void
   PushSubscriptionsTester::Test()
   {
      // The sender list. Empty is everyone; an address is exact; an @domain is a
      // suffix, so a look-alike domain that merely ends with the same letters must
      // not match.
      if (!PushSubscriptions::SenderMatches(_T(""), _T("anyone@example.com")))
         throw std::logic_error("An empty sender list must match every sender.");
      if (!PushSubscriptions::SenderMatches(_T("boss@work.example"), _T("BOSS@Work.Example")))
         throw std::logic_error("A sender list must match without regard to case.");
      if (PushSubscriptions::SenderMatches(_T("boss@work.example"), _T("someone@work.example")))
         throw std::logic_error("A sender list of one address must not match another address.");
      if (!PushSubscriptions::SenderMatches(_T("@work.example, alerts@example.net"), _T("anyone@work.example")))
         throw std::logic_error("An @domain entry must match every address in that domain.");
      if (PushSubscriptions::SenderMatches(_T("@work.example"), _T("anyone@notwork.example")))
         throw std::logic_error("An @domain entry must not match a domain that merely ends with the same letters.");
      if (PushSubscriptions::SenderMatches(_T("@work.example"), _T("")))
         throw std::logic_error("A message with no sender must not match a sender list.");

      // The labels. The message's keywords are space-separated as IMAP stores them.
      if (!PushSubscriptions::LabelMatches(_T(""), _T("")))
         throw std::logic_error("An empty label list must match a message with no labels.");
      if (!PushSubscriptions::LabelMatches(_T("Urgent"), _T("Seen urgent")))
         throw std::logic_error("A label must be matched against the message's keywords without regard to case.");
      if (PushSubscriptions::LabelMatches(_T("Urgent"), _T("Later")))
         throw std::logic_error("A label list must not match a message carrying another label.");
      if (PushSubscriptions::LabelMatches(_T("Urgent"), _T("")))
         throw std::logic_error("A label list must not match a message carrying no labels at all.");

      // Quiet hours, in the device's own time. 22:00 to 07:00, on a device two
      // hours ahead of UTC.
      PushSubscriptions::Subscription subscription;
      subscription.quiet_from = 22 * 60;
      subscription.quiet_to = 7 * 60;
      subscription.quiet_offset = 120;

      // 1788934400 is 2026-09-09 06:13:20 UTC, which is 08:13 on the device: awake.
      // (1789000000, which this read before, is 00:26 UTC on the 10th - 02:26 on
      // the device, inside the window - and the self-test failed on its own sum.)
      if (PushSubscriptions::IsQuiet(subscription, 1788934400))
         throw std::logic_error("08:13 on the device is not inside quiet hours of 22:00 to 07:00.");

      // Four hours earlier is 02:13 on the device: quiet.
      if (!PushSubscriptions::IsQuiet(subscription, 1788934400 - 6 * 3600))
         throw std::logic_error("02:13 on the device is inside quiet hours that wrap midnight.");

      // And 23:13 on the device, which is the other side of the wrap.
      if (!PushSubscriptions::IsQuiet(subscription, 1788934400 - 9 * 3600))
         throw std::logic_error("23:13 on the device is inside quiet hours that wrap midnight.");

      // A window that does not wrap: 13:00 to 14:00, on a device on UTC.
      PushSubscriptions::Subscription plain;
      plain.quiet_from = 13 * 60;
      plain.quiet_to = 14 * 60;
      plain.quiet_offset = 0;
      if (!PushSubscriptions::IsQuiet(plain, 1788934400 + 7 * 3600))
         throw std::logic_error("13:13 UTC is inside quiet hours of 13:00 to 14:00.");
      if (PushSubscriptions::IsQuiet(plain, 1788934400 + 8 * 3600))
         throw std::logic_error("14:13 UTC is not inside quiet hours that end at 14:00.");

      // No quiet hours at all, and the degenerate window.
      PushSubscriptions::Subscription always;
      if (PushSubscriptions::IsQuiet(always, 1788934400))
         throw std::logic_error("A device with no quiet hours is never quiet.");
      always.quiet_from = 60;
      always.quiet_to = 60;
      if (PushSubscriptions::IsQuiet(always, 1788934400))
         throw std::logic_error("Quiet hours whose ends are the same are no quiet hours, not a whole day of them.");

      // A device behind UTC, where the naive modulus would go negative.
      PushSubscriptions::Subscription behind;
      behind.quiet_from = 22 * 60;
      behind.quiet_to = 7 * 60;
      behind.quiet_offset = -8 * 60;
      // 1788934400 is 06:13 UTC, so 22:13 the previous day on the device.
      if (!PushSubscriptions::IsQuiet(behind, 1788934400))
         throw std::logic_error("A device behind UTC must have its own minute of the day worked out, not a negative one.");

      // And the whole decision together: a dead-lettered device is told nothing
      // whatever its choices say.
      PushSubscriptions::Subscription dead;
      dead.state = PushSubscriptions::StateDeadLettered;
      if (PushSubscriptions::Wants(dead, _T("a@example.com"), _T(""), 1788934400))
         throw std::logic_error("A dead-lettered subscription is sent nothing.");

      PushSubscriptions::Subscription live;
      live.senders = _T("@work.example");
      if (!PushSubscriptions::Wants(live, _T("boss@work.example"), _T(""), 1788934400))
         throw std::logic_error("A live subscription whose sender list matches is told.");
      if (PushSubscriptions::Wants(live, _T("spam@elsewhere.example"), _T(""), 1788934400))
         throw std::logic_error("A live subscription whose sender list does not match is not told.");
   }
}
