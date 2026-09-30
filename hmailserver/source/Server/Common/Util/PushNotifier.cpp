// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "PushNotifier.h"

#include "FileUtilities.h"
#include "HttpsClient.h"
#include "PushSubscriptions.h"
#include "Unicode.h"
#include "WebPush.h"
#include "../Application/Application.h"
#include "../Application/Logger.h"
#include "../Application/Configuration.h"
#include "../BO/Account.h"
#include "../BO/Message.h"
#include "../Cache/CacheContainer.h"
#include "../Mime/Mime.h"
#include "../Persistence/MessageStore.h"
#include "../Persistence/PersistentMessage.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../Threading/WorkQueue.h"

#include <map>
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
      // The folders new mail is NOT announced from. The bits are
      // IMAPSpecialUse::Designation's - Drafts, Junk, Sent and Trash - named here
      // rather than included, because this file is Common and that header is
      // IMAP's. A message a rule filed as junk is not news; a draft the webmail
      // saved is not news either, and neither is a copy in Sent.
      const int DesignationDrafts = 0x04;
      const int DesignationJunk   = 0x10;
      const int DesignationSent   = 0x20;
      const int DesignationTrash  = 0x40;
      const int NotAnnounced = DesignationDrafts | DesignationJunk | DesignationSent | DesignationTrash;

      // A note is kept this long. Longer than the longest backoff, so that a push
      // service that was down for an hour still delivers what was waiting; not so
      // long that a phone is told at breakfast about mail from the night before,
      // which is what the time to live on the push service is for anyway.
      const __int64 NoteLifetimeSeconds = 2 * 60 * 60;

      // The list is bounded. A server delivering a thousand messages a second to
      // one subscribed account must not grow this without limit, and dropping the
      // oldest is right: a push is about what just arrived.
      const size_t PendingMaximum = 5000;

      // What a payload may carry of a person's mail, in characters. A subject
      // longer than this is cut with an ellipsis; a notification is a glance.
      const int SubjectMaximum = 120;
      const int SenderMaximum = 120;

      // RFC 8292: a token is valid for at most 24 hours. Twelve, so that a clock a
      // few minutes out on either side is never the reason a push is refused.
      const __int64 TokenLifetimeSeconds = 12 * 60 * 60;

      // How long one push may take. The drain sends every account's devices in
      // turn, so a push service that answers slowly holds everybody's behind it;
      // the test holds one of the REST server's four workers. A push service in
      // the field answers in well under a second.
      const int DrainTimeoutSeconds = 10;
      const int TestTimeoutSeconds = 5;

      // A push service's answer is a status and a line or two.
      const size_t MaximumAnswerBytes = 32 * 1024;

      AnsiString Utf8Of(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);
         return utf8;
      }

      String Shorten(const String &value, int maximum)
      {
         String text = value;
         text.Trim();

         // One line: a subject with a newline in it would be two lines of a
         // notification, and the second could say anything.
         text.Replace(_T("\r"), _T(" "));
         text.Replace(_T("\n"), _T(" "));

         if (text.GetLength() > maximum)
            text = text.Mid(0, maximum - 1) + _T("\u2026");

         return text;
      }

      // "Ada Lovelace <ada@example.com>" -> the two halves. A From header that is
      // just an address gives an empty name.
      void SplitFrom(const String &from, String &name, String &address)
      {
         name.Empty();
         address = from;
         address.Trim();

         int open = address.ReverseFind('<');
         int close = address.ReverseFind('>');

         if (open >= 0 && close > open)
         {
            name = address.Mid(0, open);
            address = address.Mid(open + 1, close - open - 1);
         }

         name.Trim();
         address.Trim();

         // A display name is often quoted; the quotes are not part of it.
         if (name.GetLength() >= 2 && name.StartsWith(_T("\"")) && name.EndsWith(_T("\"")))
            name = name.Mid(1, name.GetLength() - 2);
      }
   }

   PushNotifier::PushNotifier()
      : draining_(false)
   {
   }

   PushNotifier::~PushNotifier()
   {
   }

   int
   PushNotifier::BackoffMinutes(int attemptsSoFar)
   {
      // The alert webhook's table, and for the same reason: the commonest cause of
      // a first failure is a push service that was restarting or a name that had
      // not resolved, so the first retry is on the next pass rather than after a
      // wait.
      switch (attemptsSoFar)
      {
      case 0:  return 0;
      case 1:  return 1;
      case 2:  return 5;
      case 3:  return 15;
      default: return 60;
      }
   }

   AnsiString
   PushNotifier::VapidSubject()
   {
      String subject = Configuration::Instance()->GetWebPushSubject();
      subject.Trim();

      if (!subject.IsEmpty())
         return AnsiString(subject);

      String host = Configuration::Instance()->GetHostName();
      host.Trim();

      if (host.IsEmpty())
         return AnsiString();

      return AnsiString(_T("mailto:postmaster@") + host);
   }

   AnsiString
   PushNotifier::BuildPayload(int detail, int count, const AnsiString &sender, const AnsiString &name,
                              const AnsiString &subject, const AnsiString &folder, __int64 when)
   {
      // Every field of this document is listed in docs/WebPush.md. Nothing is
      // added here without being added there: the promise the feature makes is
      // that a push carries exactly what the person allowed and nothing else.
      AnsiString json = "{\"v\":1,\"kind\":\"mail\"";

      AnsiString numbers;
      numbers.Format(",\"count\":%d,\"at\":%I64d", count, when);
      json += numbers;

      json += ",\"folder\":\"" + WebPush::JsonEscape(folder) + "\"";

      if (detail >= PushSubscriptions::DetailSender)
      {
         json += ",\"from\":\"" + WebPush::JsonEscape(sender) + "\"";
         json += ",\"name\":\"" + WebPush::JsonEscape(name) + "\"";
      }

      if (detail >= PushSubscriptions::DetailSubject)
         json += ",\"subject\":\"" + WebPush::JsonEscape(subject) + "\"";

      json += "}";
      return json;
   }

   void
   PushNotifier::Note(__int64 accountId, std::shared_ptr<const Message> message)
   {
      if (!message || accountId <= 0)
         return;

      // The switch is read here rather than in the task, so that a server with the
      // feature off does not even keep a list. The read is one property lookup in
      // memory.
      if (!Configuration::Instance()->GetWebPushEnabled())
         return;

      Pending note;
      note.account_id = accountId;
      note.folder_id = message->GetFolderID();
      note.message_id = message->GetID();
      note.noted = (__int64) time(nullptr);
      note.sender = message->GetFromAddress();
      note.keywords = message->GetKeywords();

      // The path is taken now, while the message object is in hand. Working it out
      // later would need the account, and the account may have been deleted by
      // then - in which case there is nothing to announce anyway. The account is
      // what places the file: without it the path is the data directory's root,
      // where no delivered message is, and the subject would read as empty.
      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(accountId);
      note.file_name = account ? PersistentMessage::GetFileName(account, message)
                               : PersistentMessage::GetFileName(message);

      bool post = false;

      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);

         if (pending_.size() >= PendingMaximum)
            pending_.pop_front();

         pending_.push_back(note);

         if (!draining_)
         {
            draining_ = true;
            post = true;
         }
      }

      if (!post)
         return;

      // Off the delivery thread at once. A failure to post is not a failure of the
      // delivery: the minute-by-minute task will find the note.
      std::shared_ptr<WorkQueue> queue = Application::Instance()->GetAsyncWorkQueue();
      if (!queue)
      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);
         draining_ = false;
         return;
      }

      std::shared_ptr<PushDrainTask> task = std::shared_ptr<PushDrainTask>(new PushDrainTask());
      queue->AddTask(task);
   }

   size_t
   PushNotifier::PendingCount()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);
      return pending_.size();
   }

   void
   PushNotifier::Forget()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);
      pending_.clear();
   }

   void
   PushNotifier::DrainStarting()
   {
      boost::lock_guard<boost::recursive_mutex> guard(mutex_);
      draining_ = false;
   }

   bool
   PushNotifier::FolderIsAnnounced_(__int64 folderId, String &name)
   {
      name.Empty();

      SQLCommand command(_T("select foldername, folderspecialuse from hm_imapfolders where folderid = @ID"));
      command.AddParameter("@ID", folderId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      name = recordset->GetStringValue("foldername");
      long designations = recordset->GetLongValue("folderspecialuse");

      return (designations & NotAnnounced) == 0;
   }

   String
   PushNotifier::SubjectOf_(const String &fileName)
   {
      if (fileName.IsEmpty() || !MessageStore::Exists(fileName))
         return String();

      AnsiString header = PersistentMessage::LoadHeader(fileName, false);
      if (header.IsEmpty())
         return String();

      MimeHeader mimeHeader;
      mimeHeader.Load(header.c_str(), header.GetLength(), true);

      return Shorten(mimeHeader.GetUnicodeFieldValue("Subject"), SubjectMaximum);
   }

   bool
   PushNotifier::SendOne_(const PushSubscriptions::Subscription &subscription, const AnsiString &payload,
                          int timeoutSeconds, int &statusCode, String &error)
   {
      statusCode = 0;
      error.Empty();

      String privateKey = Configuration::Instance()->GetWebPushVapidPrivateKey();
      AnsiString publicKey = AnsiString(Configuration::Instance()->GetWebPushVapidPublicKey());

      if (privateKey.IsEmpty() || publicKey.IsEmpty())
      {
         error = _T("this server has no VAPID key pair, so no push service would accept anything from it");
         return false;
      }

      AnsiString authorization;
      __int64 expiry = (__int64) time(nullptr) + TokenLifetimeSeconds;

      if (!WebPush::BuildVapidHeader(AnsiString(subscription.endpoint), VapidSubject(), expiry,
                                     publicKey, AnsiString(privateKey), authorization, error))
         return false;

      AnsiString body;
      if (!WebPush::Encrypt(payload, AnsiString(subscription.p256dh), AnsiString(subscription.auth), body, error))
         return false;

      std::vector<AnsiString> headers;

      AnsiString ttl;
      ttl.Format("TTL: %d", Configuration::Instance()->GetWebPushTimeToLiveSeconds());
      headers.push_back(ttl);
      headers.push_back("Content-Encoding: aes128gcm");
      headers.push_back("Urgency: normal");
      headers.push_back("Authorization: " + authorization);

      HttpsClient::Response response;

      // The endpoint is whatever a person's browser - or the person - handed
      // over, so it is reached only at a public address (RequestPublic), with
      // the one exception PushSubscriptions::Save takes from this machine
      // alone: plain http to its own loopback. The reason a request failed is
      // the administrator's, in the application log; what the person is shown
      // says only that it failed, since "refused", "timed out" and a TLS error
      // from an address they chose would map the network for them.
      String transportError;
      // A loopback endpoint saved while the regression suite's switch was on is
      // sent to with RequestPublic once it is off, which refuses it.
      const bool loopbackForTests = PushSubscriptions::IsLoopbackHttp(subscription.endpoint) &&
                                    Configuration::Instance()->GetWebPushLoopbackHttpForTests();
      const bool answered = loopbackForTests
         ? HttpsClient::Request("POST", AnsiString(subscription.endpoint), headers, "application/octet-stream",
                                body, response, transportError, timeoutSeconds, MaximumAnswerBytes)
         : HttpsClient::RequestPublic("POST", AnsiString(subscription.endpoint), headers, "application/octet-stream",
                                      body, response, transportError, timeoutSeconds, MaximumAnswerBytes);
      if (!answered)
      {
         AnsiString audience;
         WebPush::AudienceOf(AnsiString(subscription.endpoint), audience);
         LOG_APPLICATION(Formatter::Format("Web Push: the push service {0} for device \"{1}\" was not reached: {2}",
            String(audience), subscription.device, transportError));
         error = _T("the push service could not be reached");
         return false;
      }

      statusCode = response.status_code;

      if (response.status_code < 200 || response.status_code > 299)
      {
         error.Format(_T("the push service answered %d"), response.status_code);
         return false;
      }

      return true;
   }

   bool
   PushNotifier::SendTest(const PushSubscriptions::Subscription &subscription, String &error)
   {
      error.Empty();

      if (!Configuration::Instance()->GetWebPushEnabled())
      {
         error = _T("Web Push is switched off on this server.");
         return false;
      }

      // At the count level whatever the device allows: a test is about no
      // message, so it names nobody and carries no subject, empty or otherwise.
      AnsiString payload = BuildPayload(PushSubscriptions::DetailCount, 1, "", "", "", "", (__int64) time(nullptr));

      // A test says it is a test, so that the page can word it as one and a person
      // is never left wondering which message it was about.
      payload = payload.Mid(0, payload.GetLength() - 1) + ",\"test\":true}";

      // A shorter deadline than the drain's: this one holds a REST worker, of
      // which there are four, while it waits.
      int statusCode = 0;
      if (SendOne_(subscription, payload, TestTimeoutSeconds, statusCode, error))
      {
         PushSubscriptions::MarkDelivered(subscription.id, (__int64) time(nullptr));
         return true;
      }

      if (statusCode == 404 || statusCode == 410)
      {
         PushSubscriptions::Remove(subscription.account_id, subscription.id);
         error = _T("The push service says this device's subscription is gone, so it has been removed. Turn notifications on again on that device.");
         return false;
      }

      // A test that failed is a failure like any other: counted against the
      // subscription with the reason shown, and a next attempt set, so that the
      // page says what happened rather than showing a device that looks fine and
      // never hears anything.
      RecordFailure_(subscription.id, subscription.failures, (__int64) time(nullptr), error);

      return false;
   }

   void
   PushNotifier::RecordFailure_(__int64 subscriptionId, int failuresSoFar, __int64 now, const String &error)
   {
      const int failures = failuresSoFar + 1;
      const int maximumAttempts = Configuration::Instance()->GetWebPushMaxAttempts();

      if (failures >= maximumAttempts)
      {
         PushSubscriptions::MarkFailed(subscriptionId, failures, 0, PushSubscriptions::StateDeadLettered, error);
         return;
      }

      const __int64 next = now + (__int64) BackoffMinutes(failures - 1) * 60;
      PushSubscriptions::MarkFailed(subscriptionId, failures, next, PushSubscriptions::StateActive, error);
   }

   int
   PushNotifier::SendPending()
   {
      if (!Configuration::Instance()->GetWebPushEnabled())
      {
         Forget();
         return 0;
      }

      std::deque<Pending> due;
      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);
         due.swap(pending_);
      }

      if (due.empty())
         return 0;

      __int64 now = (__int64) time(nullptr);
      int sent = 0;

      // Folders and subscriptions are read once per pass and not once per message:
      // a hundred messages to one account in a minute is one read of its devices.
      std::map<__int64, std::pair<bool, String> > folders;
      std::map<__int64, std::vector<PushSubscriptions::Subscription> > devices;

      // What is put back: a note whose devices are all backing off, so that it is
      // tried again on the next pass rather than lost.
      std::deque<Pending> again;

      for (const Pending &note : due)
      {
         if (now - note.noted > NoteLifetimeSeconds)
            continue;

         auto folder = folders.find(note.folder_id);
         if (folder == folders.end())
         {
            String name;
            bool announced = FolderIsAnnounced_(note.folder_id, name);
            folder = folders.insert(std::make_pair(note.folder_id, std::make_pair(announced, name))).first;
         }

         if (!folder->second.first)
            continue;

         auto account = devices.find(note.account_id);
         if (account == devices.end())
         {
            std::vector<PushSubscriptions::Subscription> subscriptions;
            PushSubscriptions::ForAccount(note.account_id, subscriptions);
            account = devices.insert(std::make_pair(note.account_id, subscriptions)).first;
         }

         if (account->second.empty())
            continue;

         String name;
         String address;
         SplitFrom(note.sender, name, address);

         // Read only when a device is allowed to be told it: a subject nobody may
         // be told is a file this server does not open.
         bool subjectRead = false;
         String subject;

         bool deferred = false;

         for (PushSubscriptions::Subscription &subscription : account->second)
         {
            if (!PushSubscriptions::Wants(subscription, address, note.keywords, now))
               continue;

            if (subscription.next_attempt > now)
            {
               deferred = true;
               continue;
            }

            if (subscription.detail >= PushSubscriptions::DetailSubject && !subjectRead)
            {
               subject = SubjectOf_(note.file_name);
               subjectRead = true;
            }

            AnsiString payload = BuildPayload(subscription.detail, 1,
                                              Utf8Of(Shorten(address, SenderMaximum)),
                                              Utf8Of(Shorten(name, SenderMaximum)),
                                              Utf8Of(subject),
                                              Utf8Of(folder->second.second),
                                              note.noted);

            int statusCode = 0;
            String error;

            if (SendOne_(subscription, payload, DrainTimeoutSeconds, statusCode, error))
            {
               PushSubscriptions::MarkDelivered(subscription.id, now);
               subscription.failures = 0;
               subscription.next_attempt = 0;
               sent++;
               continue;
            }

            // The push service saying the subscription is gone is not a failure to
            // retry. 404 and 410 both mean it: RFC 8030 uses 404 for an endpoint
            // that never existed and 410 for one that has expired, and every push
            // service in the field uses both.
            if (statusCode == 404 || statusCode == 410)
            {
               PushSubscriptions::Remove(subscription.account_id, subscription.id);
               subscription.state = PushSubscriptions::StateDeadLettered;   // so this pass stops using it

               LOG_APPLICATION(Formatter::Format("Web Push: the subscription for device \"{0}\" answered {1} and has been removed.",
                  subscription.device, statusCode));
               continue;
            }

            int failures = subscription.failures + 1;
            int maximumAttempts = Configuration::Instance()->GetWebPushMaxAttempts();

            if (failures >= maximumAttempts)
            {
               PushSubscriptions::MarkFailed(subscription.id, failures, 0, PushSubscriptions::StateDeadLettered, error);
               subscription.state = PushSubscriptions::StateDeadLettered;

               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7002, "PushNotifier::SendPending",
                  Formatter::Format("Web Push: nothing has reached the device \"{0}\" after {1} attempts, so it has been dead-lettered and nothing more will be sent to it. The last failure was: {2}. The subscription is still listed in the webmail, with the reason, and turning notifications on again on that device replaces it.",
                     subscription.device, failures, error));
               continue;
            }

            __int64 next = now + (__int64) BackoffMinutes(failures - 1) * 60;
            PushSubscriptions::MarkFailed(subscription.id, failures, next, PushSubscriptions::StateActive, error);
            subscription.failures = failures;
            subscription.next_attempt = next;
            deferred = true;
         }

         if (deferred)
            again.push_back(note);
      }

      if (!again.empty())
      {
         boost::lock_guard<boost::recursive_mutex> guard(mutex_);
         for (const Pending &note : again)
         {
            if (pending_.size() >= PendingMaximum)
               pending_.pop_front();

            pending_.push_back(note);
         }
      }

      return sent;
   }

   PushTask::PushTask()
   {
   }

   PushTask::~PushTask()
   {
   }

   void
   PushTask::DoWork()
   {
      PushNotifier::Instance()->SendPending();
   }

   PushDrainTask::PushDrainTask()
      : Task(_T("Web Push drain"))
   {
   }

   void
   PushDrainTask::DoWork()
   {
      // The flag is cleared BEFORE the pass rather than after it, so that a
      // message delivered while this pass is running posts another drain of its
      // own. Clearing it afterwards would hold that message's notification until
      // the task's next minute, which is the whole thing this drain exists to
      // avoid - at the cost, at worst, of one extra pass that finds nothing.
      PushNotifier::Instance()->DrainStarting();
      PushNotifier::Instance()->SendPending();
   }

   // ------------------------------------------------------------------------
   // The in-process tests, run by ClassTester::DoTests.
   // ------------------------------------------------------------------------

   void
   PushNotifierTester::Test()
   {
      // The backoff table the documentation states.
      if (PushNotifier::BackoffMinutes(0) != 0 || PushNotifier::BackoffMinutes(1) != 1 ||
          PushNotifier::BackoffMinutes(2) != 5 || PushNotifier::BackoffMinutes(3) != 15 ||
          PushNotifier::BackoffMinutes(4) != 60 || PushNotifier::BackoffMinutes(40) != 60)
         throw std::logic_error("PushNotifier::BackoffMinutes is not 0, 1, 5, 15, 60, 60 ...");

      // The payload at each level of detail. What is NOT there is the assertion
      // that matters: a device told only a count must not be sent an address.
      AnsiString counted = PushNotifier::BuildPayload(PushSubscriptions::DetailCount, 2, "ada@example.com",
                                                      "Ada Lovelace", "The engine", "INBOX", 1789000000);
      if (counted.Find("ada@example.com") >= 0 || counted.Find("Ada Lovelace") >= 0 || counted.Find("The engine") >= 0)
         throw std::logic_error("A push at the count level carried the sender or the subject.");
      if (counted.Find("\"count\":2") < 0 || counted.Find("\"folder\":\"INBOX\"") < 0)
         throw std::logic_error("A push at the count level must still carry the count and the folder.");

      AnsiString withSender = PushNotifier::BuildPayload(PushSubscriptions::DetailSender, 1, "ada@example.com",
                                                        "Ada Lovelace", "The engine", "INBOX", 1789000000);
      if (withSender.Find("ada@example.com") < 0 || withSender.Find("Ada Lovelace") < 0)
         throw std::logic_error("A push at the sender level must carry the sender.");
      if (withSender.Find("The engine") >= 0)
         throw std::logic_error("A push at the sender level carried the subject.");

      AnsiString withSubject = PushNotifier::BuildPayload(PushSubscriptions::DetailSubject, 1, "ada@example.com",
                                                         "Ada Lovelace", "The engine", "INBOX", 1789000000);
      if (withSubject.Find("The engine") < 0)
         throw std::logic_error("A push at the subject level must carry the subject.");

      // A subject holding a quote and a newline must not be able to end the JSON
      // document early or forge a second field.
      AnsiString hostile = PushNotifier::BuildPayload(PushSubscriptions::DetailSubject, 1, "a@b.example", "",
                                                      "\",\"kind\":\"forged\",\"x\":\"", "INBOX", 1789000000);
      if (hostile.Find("\"kind\":\"forged\"") >= 0)
         throw std::logic_error("A subject was able to forge a field of the push payload.");
      if (hostile.Find("\\\"") < 0)
         throw std::logic_error("A quote in a subject is not escaped in the push payload.");

      // And the whole payload has to fit in one record whatever is put in it.
      if (withSubject.GetLength() >= (int) WebPush::MaximumPlainTextLength())
         throw std::logic_error("A push payload does not fit in one RFC 8188 record.");
   }
}
