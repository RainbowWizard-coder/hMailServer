// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "RspamdLearner.h"

#include "AntiSpamConfiguration.h"
#include "MoveLearning.h"

#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../Application/ErrorManager.h"
#include "../BO/Account.h"
#include "../BO/Message.h"
#include "../Persistence/MessageStore.h"
#include "../Persistence/PersistentMessage.h"
#include "../Threading/Task.h"
#include "../Threading/WorkQueue.h"
#include "../Util/Crypt.h"
#include "../Util/HttpsClient.h"
#include "../Util/Unicode.h"
#include "../Util/Strings/Formatter.h"

#include <chrono>
#include <map>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The most of Rspamd's own words a log line carries.
      const int MostDetailCharacters = 300;

      // Sends the lessons waiting, oldest first, until none is left, on a thread of
      // the asynchronous work queue: never the IMAP thread, and inside no folder
      // lock or transaction. One at a time, each under RspamdTimeoutSeconds. A task
      // the queue never ran (the servers stopped first), or one a stop
      // interrupted, tells the queue so as it goes, and the next command that
      // queues a lesson starts another.
      class RspamdLearnTask : public Task
      {
      public:
         RspamdLearnTask() :
            Task("RspamdLearnTask"),
            emptied_(false)
         {
         }

         ~RspamdLearnTask()
         {
            if (!emptied_)
               RspamdLessonQueue::Instance().DrainerEnded();
         }

         virtual void DoWork()
         {
            RspamdLearner::Lesson lesson;
            while (RspamdLessonQueue::Instance().Take(lesson))
            {
               try
               {
                  RspamdLearner::Teach(lesson);
               }
               catch (boost::thread_interrupted &)
               {
                  throw;
               }
               catch (std::exception &failure)
               {
                  // One lesson that could not be given is that lesson's alone.
                  LOG_APPLICATION(Formatter::Format("Rspamd: a lesson was not given: {0}", String(failure.what())));
               }
            }

            emptied_ = true;
         }

      private:
         bool emptied_;
      };

      // Rspamd's error text, as {"error":"..."}, or empty.
      String ErrorText(const AnsiString &body)
      {
         const AnsiString key = "\"error\"";
         int at = body.Find(key);
         if (at < 0)
            return String();

         int colon = body.Find(":", at + key.GetLength());
         if (colon < 0)
            return String();

         int i = colon + 1;
         while (i < body.GetLength() && (body.GetAt(i) == ' ' || body.GetAt(i) == '\t' || body.GetAt(i) == '\r' || body.GetAt(i) == '\n'))
            i++;

         if (i >= body.GetLength() || body.GetAt(i) != '"')
            return String();

         i++;
         AnsiString text;
         while (i < body.GetLength() && body.GetAt(i) != '"')
         {
            if (body.GetAt(i) == '\\' && i + 1 < body.GetLength())
               i++;

            text += body.GetAt(i);
            i++;
         }

         return String(text);
      }

      // Text from the answer made fit for one log line: printable ASCII, no line
      // breaks, and not too long.
      String SafeForLog(const String &text)
      {
         String safe;
         for (int i = 0; i < text.GetLength() && safe.GetLength() < MostDetailCharacters; i++)
         {
            const wchar_t c = text.GetAt(i);
            safe += (c >= 0x20 && c < 0x7F) ? c : L'?';
         }

         return safe;
      }

      bool HasSuccessTrue(const AnsiString &body)
      {
         AnsiString compact;
         for (int i = 0; i < body.GetLength(); i++)
         {
            const char c = body.GetAt(i);
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
               compact += c;
         }

         return compact.Find("\"success\":true") >= 0;
      }

      // Visible ASCII, no space: what a URL, a password or a classifier's name
      // may hold, each of which goes into a request line or a header as it is.
      bool VisibleAscii(const String &text)
      {
         for (int i = 0; i < text.GetLength(); i++)
         {
            const wchar_t c = text.GetAt(i);
            if (c <= 0x20 || c >= 0x7F)
               return false;
         }

         return true;
      }
   }

   String
   RspamdLearner::UrlProblem(const String &url, const String &settingName)
   {
      if (url.IsEmpty())
         return String();

      if (url.GetLength() > 2048)
         return settingName + _T(" is longer than 2048 characters.");

      if (!VisibleAscii(url))
         return settingName + _T(" contains a space, a control character or a character outside ASCII.");

      const AnsiString text = AnsiString(url);

      if (text.Find("?") >= 0 || text.Find("#") >= 0)
         return settingName + _T(" is the controller's address alone, with no query (?) or fragment (#): the password is sent in a header, never in the URL.");

      const bool https = text.Left(8) == "https://";
      const bool http = text.Left(7) == "http://";
      if (!https && !http)
         return settingName + _T(" must be an https URL, written in lower case - or plain http to this machine (127.0.0.1, ::1 or localhost), such as http://127.0.0.1:11334.");

      const int authorityStart = https ? 8 : 7;
      const int pathStart = text.Find("/", static_cast<size_t>(authorityStart));
      const AnsiString authority = pathStart < 0 ? text.Mid(authorityStart) : text.Mid(authorityStart, pathStart - authorityStart);
      if (authority.Find("@") >= 0)
         return settingName + _T(" must not carry a user name or a password; the password is a setting of its own.");

      bool parsedHttps = false;
      AnsiString host, port, path;
      if (!HttpsClient::ParseUrl(text, parsedHttps, host, port, path) || host.IsEmpty())
         return settingName + _T(" is not an http or https URL with a host.");

      const long portNumber = atol(port.c_str());
      if (port.GetLength() > 5 || portNumber < 1 || portNumber > 65535)
         return settingName + _T("'s port must be between 1 and 65535.");

      if (!https && !HttpsClient::IsLoopbackHost(host))
         return settingName + _T(" must be an https URL: plain http is accepted only to this machine (127.0.0.1, ::1 or localhost), since the controller's password and every message taught would otherwise cross the network in the clear.");

      return String();
   }

   String
   RspamdLearner::PasswordProblem(const String &password, const String &settingName)
   {
      if (password.IsEmpty())
         return String();

      if (password.GetLength() > 128)
         return settingName + _T(" is longer than 128 characters.");

      if (!VisibleAscii(password))
         return settingName + _T(" may hold only visible ASCII characters - no space, tab or line break, and nothing outside ASCII - since it is sent in the Password header.");

      return String();
   }

   String
   RspamdLearner::PasswordStoreProblem(const String &password, const String &settingName)
   {
      const String problem = PasswordProblem(password, settingName);
      if (!problem.IsEmpty() || password.IsEmpty())
         return problem;

      // What the store would hold: the platform's protection, or the legacy
      // scheme while ProtectStoredSecretsWithDPAPI is 0, which holds 248
      // characters - more than the 128 allowed - and on Linux the key file,
      // without which nothing is stored.
      const String stored = Crypt::Instance()->ProtectSecret(password);
      if (stored.IsEmpty())
         return settingName + _T(" could not be stored protected; the error log says why.");

      if (stored.GetLength() > 4000)
         return settingName + _T(" is too long to store protected.");

      return String();
   }

   String
   RspamdLearner::ClassifierProblem(const String &classifier, const String &settingName)
   {
      if (classifier.IsEmpty())
         return String();

      if (classifier.GetLength() > 128)
         return settingName + _T(" is longer than 128 characters.");

      if (!VisibleAscii(classifier))
         return settingName + _T(" is the name a classifier has in Rspamd's configuration: visible ASCII, no space.");

      return String();
   }

   String
   RspamdLearner::TimeoutProblem(long seconds, const String &settingName)
   {
      if (seconds < 1 || seconds > 300)
         return settingName + _T(" must be between 1 and 300 seconds.");

      return String();
   }

   bool
   RspamdLearner::BuildRequest(const String &controllerUrl, const String &password, const String &classifier,
                               const String &owner, bool spam, Request &request, String &problem)
   {
      request = Request();
      problem = String();

      String url = controllerUrl;
      url.Trim();

      if (url.IsEmpty())
      {
         problem = _T("RspamdControllerUrl is empty");
         return false;
      }

      problem = UrlProblem(url, _T("RspamdControllerUrl"));
      if (!problem.IsEmpty())
         return false;

      if (classifier.IsEmpty())
      {
         problem = _T("RspamdClassifier is empty, and without a classifier named Rspamd would teach every classifier it has, the global one among them");
         return false;
      }

      problem = ClassifierProblem(classifier, _T("RspamdClassifier"));
      if (!problem.IsEmpty())
         return false;

      problem = PasswordProblem(password, _T("RspamdControllerPassword"));
      if (!problem.IsEmpty())
         return false;

      // The owner's address is the one identity a lesson carries. An address is
      // never anything a header could be ended by, but it is held to that here
      // too, since it goes into two headers as it is.
      const AnsiString identity = Unicode::ToUtf8(owner);
      if (identity.IsEmpty() || identity.Find("@") < 0)
      {
         problem = _T("the mailbox owner has no address");
         return false;
      }

      for (int i = 0; i < identity.GetLength(); i++)
      {
         const unsigned char c = (unsigned char) identity.GetAt(i);
         if (c <= 0x20 || c == 0x7F || c == ',' || c == '<' || c == '>')
         {
            problem = _T("the mailbox owner's address cannot be sent in a header");
            return false;
         }
      }

      // The controller's address may end in a slash, or carry a path a reverse
      // proxy puts it under; the endpoint goes after it.
      AnsiString base = AnsiString(url);
      while (base.GetLength() > 0 && base.GetAt(base.GetLength() - 1) == '/')
         base = base.Left(base.GetLength() - 1);

      request.url = base + (spam ? "/learnspam" : "/learnham");

      if (!password.IsEmpty())
         request.headers.push_back("Password: " + AnsiString(password));

      request.headers.push_back("Classifier: " + AnsiString(classifier));
      request.headers.push_back("Deliver-To: " + identity);
      request.headers.push_back("Rcpt: " + identity);
      return true;
   }

   RspamdLearner::Outcome
   RspamdLearner::Interpret(int status, const AnsiString &body, String &detail)
   {
      detail = SafeForLog(ErrorText(body));

      // The learn cache's answer, whichever code carries it: 208 from the
      // classifier, 404 from the cache itself.
      if (detail.FindNoCase(_T("already learned")) >= 0)
         return OutcomeAlreadyLearned;

      if (status == 200)
      {
         if (HasSuccessTrue(body))
            return OutcomeLearned;

         detail = _T("the controller answered 200 without \"success\": true");
         return OutcomeFailed;
      }

      if (status == 208)
         return OutcomeAlreadyLearned;

      if (status == 204)
      {
         if (detail.IsEmpty())
            detail = _T("Rspamd declined to learn the message");
         return OutcomeDeclined;
      }

      if (status == 401 || status == 403)
      {
         if (detail.IsEmpty())
            detail = _T("Unauthorized");
         return OutcomePasswordRefused;
      }

      if (status == 404)
      {
         if (detail.IsEmpty())
            detail = _T("not found - the URL may not be Rspamd's controller");
         return OutcomeNoClassifier;
      }

      if (detail.IsEmpty())
         detail = Formatter::Format(_T("the controller answered {0}"), status);
      else
         detail = Formatter::Format(_T("{0} ({1})"), detail, status);

      return OutcomeFailed;
   }

   String
   RspamdLearner::OutcomeText(Outcome outcome)
   {
      switch (outcome)
      {
      case OutcomeLearned:
         return _T("learned");
      case OutcomeAlreadyLearned:
         return _T("already learned as that, nothing changed");
      case OutcomeDeclined:
         return _T("declined");
      case OutcomePasswordRefused:
         return _T("refused: the controller's password");
      case OutcomeNoClassifier:
         return _T("refused: no such classifier or endpoint");
      case OutcomeFailed:
         return _T("not learned");
      }

      return _T("not learned");
   }

   RspamdLearner::Outcome
   RspamdLearner::Send(const Request &request, const AnsiString &message, int timeoutSeconds, String &detail)
   {
      detail = String();

      HttpsClient::Response response;
      String error;

      // An endpoint the administrator named, perhaps on this server's network,
      // never a link-local or metadata address and never through HttpProxy; https
      // verified, plain http only to this machine.
      if (!HttpsClient::RequestNotLinkLocal("POST", request.url, request.headers, "message/rfc822", message, response, error,
                                            timeoutSeconds, MaxAnswerBytes))
      {
         detail = SafeForLog(error.IsEmpty() ? String(_T("no answer")) : error);
         return OutcomeFailed;
      }

      return Interpret(response.status_code, response.body, detail);
   }

   namespace
   {
      // When each misconfiguration was last reported.
      boost::mutex reported_mutex;
      std::map<int, std::chrono::steady_clock::time_point> reported;
   }

   void
   RspamdLearner::ForgetReports()
   {
      boost::lock_guard<boost::mutex> guard(reported_mutex);
      reported.clear();
   }

   void
   RspamdLearner::ReportMisconfiguration_(int code, const String &description)
   {
      // Once in ten minutes for each code, and again after a setting of the
      // backend has changed (ForgetReports): every lesson meets the same mistake,
      // and the error log is for the administrator to read, not for every move a
      // user makes. The application log has each lesson.
      const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
      {
         boost::lock_guard<boost::mutex> guard(reported_mutex);
         auto last = reported.find(code);
         if (last != reported.end() && now - last->second < std::chrono::minutes(10))
            return;

         reported[code] = now;
      }

      ErrorManager::Instance()->ReportError(ErrorManager::Medium, code, "RspamdLearner::Teach", description);
   }

   void
   RspamdLearner::Queue(const std::vector<Lesson> &lessons)
   {
      if (lessons.empty())
         return;

      bool startDrainer = false;
      const size_t admitted = RspamdLessonQueue::Instance().Offer(lessons, startDrainer);

      if (admitted < lessons.size())
      {
         // The client is not made to wait for Rspamd: the newest are dropped, and
         // said so, once for the command - and for a webmail move once for each
         // slice of at most fifty messages, whose lessons are queued together.
         LOG_APPLICATION(Formatter::Format("Rspamd: {0} of the {1} lessons of a move were not queued, because {2} are waiting already, the most there may be. "
            "The controller is slower than the moves it is told of, or not answering.",
            (unsigned __int64) (lessons.size() - admitted), (unsigned __int64) lessons.size(),
            (unsigned __int64) RspamdLessonQueue::Instance().GetCapacity()));
      }

      if (!startDrainer)
         return;

      std::shared_ptr<WorkQueue> workQueue = Application::Instance()->GetAsyncWorkQueue();
      if (!workQueue)
      {
         // The servers are stopping or stopped. What was queued waits, and the
         // next command that queues a lesson starts the drainer.
         RspamdLessonQueue::Instance().DrainerEnded();
         return;
      }

      // It waits on the network, for as long as the timeout: TaskMayBlock keeps it
      // within the queue's cap for such tasks.
      workQueue->AddTask(std::shared_ptr<Task>(new RspamdLearnTask()), WorkQueue::TaskMayBlock);
   }

   void
   RspamdLearner::Teach(const Lesson &lesson)
   {
      if (!lesson.copy || !lesson.owner)
         return;

      const String owner = lesson.owner->GetAddress();
      const String kind = lesson.spam ? _T("spam") : _T("ham");
      const __int64 messageId = lesson.copy->GetID();

      AntiSpamConfiguration &config = Configuration::Instance()->GetAntiSpamConfiguration();

      // Switched to another backend since the move: what was waiting is let go.
      if (config.GetMoveLearningBackend() != MoveLearning::BackendRspamd)
      {
         LOG_APPLICATION(Formatter::Format("Rspamd: message {0} of {1} was not taught as {2}: MoveLearningBackend was changed after the move.", messageId, owner, kind));
         return;
      }

      Request request;
      String problem;
      if (!BuildRequest(config.GetRspamdControllerUrl(), config.GetRspamdControllerPassword(), config.GetRspamdClassifier(),
                        owner, lesson.spam, request, problem))
      {
         LOG_APPLICATION(Formatter::Format("Rspamd: message {0} of {1} was not taught as {2}: {3}.", messageId, owner, kind, problem));

         if (config.GetRspamdControllerUrl().IsEmpty() || !UrlProblem(config.GetRspamdControllerUrl(), _T("RspamdControllerUrl")).IsEmpty())
         {
            ReportMisconfiguration_(8580, _T("MoveLearningBackend is Rspamd, and RspamdControllerUrl is empty or not an address this server sends to (https, or plain http to this machine): no move into or out of a Junk folder teaches Rspamd until it is set."));
         }
         else if (config.GetRspamdClassifier().IsEmpty() || !ClassifierProblem(config.GetRspamdClassifier(), _T("RspamdClassifier")).IsEmpty())
         {
            ReportMisconfiguration_(8581, _T("MoveLearningBackend is Rspamd, and RspamdClassifier names no classifier: no move into or out of a Junk folder teaches Rspamd until it names one - a per-user classifier, unless every user's moves are to teach the whole server."));
         }

         return;
      }

      const String messageFile = PersistentMessage::GetFileName(lesson.owner, lesson.copy);

      // A message of a mailbox encrypted at rest is never sent out in the clear to
      // another program, as it is never written out for SpamAssassin
      // (docs/EncryptionAtRest.md): such a move teaches nothing.
      std::string publicKey;
      if (MessageStore::IsEncrypted(messageFile, publicKey))
      {
         LOG_APPLICATION(Formatter::Format("Rspamd: message {0} of {1} was not taught as {2}: the mailbox is encrypted at rest, and its messages are not sent to another program.", messageId, owner, kind));
         return;
      }

      unsigned __int64 size = 0;
      if (!MessageStore::FileSize64(messageFile, size))
      {
         LOG_APPLICATION(Formatter::Format("Rspamd: message {0} of {1} was not taught as {2}: it is no longer there - deleted since it was moved, perhaps.", messageId, owner, kind));
         return;
      }

      if (size > MaxMessageBytes)
      {
         LOG_APPLICATION(Formatter::Format("Rspamd: message {0} of {1} was not taught as {2}: it is {3} bytes, more than the {4} Rspamd takes by default.",
            messageId, owner, kind, size, (unsigned __int64) MaxMessageBytes));
         return;
      }

      AnsiString bytes;
      if (!MessageStore::ReadAll(messageFile, bytes) || bytes.IsEmpty())
      {
         LOG_APPLICATION(Formatter::Format("Rspamd: message {0} of {1} was not taught as {2}: it could not be read.", messageId, owner, kind));
         return;
      }

      int timeout = config.GetRspamdTimeoutSeconds();
      if (!TimeoutProblem(timeout, _T("RspamdTimeoutSeconds")).IsEmpty())
         timeout = 15;

      String detail;
      const Outcome outcome = Send(request, bytes, timeout, detail);

      // The outcome of every lesson, one line each: what was taught, for whom, and
      // what Rspamd made of it - never the password, and none of the message.
      const String result = OutcomeText(outcome) + (detail.IsEmpty() ? String() : String(_T(" - ")) + detail);
      LOG_APPLICATION(Formatter::Format("Rspamd was taught message {0} of {1} as {2} (classifier {3}): {4}.",
         messageId, owner, kind, config.GetRspamdClassifier(), result));

      if (outcome == OutcomePasswordRefused)
      {
         ReportMisconfiguration_(8582, _T("Rspamd's controller refused the password a lesson carried (RspamdControllerPassword): learning is a privileged command, checked against the controller's enable_password, or its password when it has no enable_password. No move teaches Rspamd until the two agree."));
      }
      else if (outcome == OutcomeNoClassifier)
      {
         ReportMisconfiguration_(8583, String(_T("Rspamd's controller answered a lesson with not found: ")) + detail +
            _T(". RspamdClassifier must name a classifier of Rspamd's statistics configuration, and RspamdControllerUrl its controller (port 11334 by default)."));
      }
   }

   RspamdLessonQueue::RspamdLessonQueue(size_t capacity) :
      capacity_(capacity),
      drainer_running_(false)
   {
   }

   size_t
   RspamdLessonQueue::Offer(const std::vector<RspamdLearner::Lesson> &lessons, bool &startDrainer)
   {
      startDrainer = false;

      boost::lock_guard<boost::mutex> guard(mutex_);

      size_t admitted = 0;
      for (const RspamdLearner::Lesson &lesson : lessons)
      {
         if (pending_.size() >= capacity_)
            break;

         pending_.push_back(lesson);
         admitted++;
      }

      if (admitted > 0 && !drainer_running_)
      {
         drainer_running_ = true;
         startDrainer = true;
      }

      return admitted;
   }

   bool
   RspamdLessonQueue::Take(RspamdLearner::Lesson &lesson)
   {
      boost::lock_guard<boost::mutex> guard(mutex_);

      if (pending_.empty())
      {
         drainer_running_ = false;
         return false;
      }

      lesson = pending_.front();
      pending_.pop_front();
      return true;
   }

   void
   RspamdLessonQueue::DrainerEnded()
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      drainer_running_ = false;
   }

   size_t
   RspamdLessonQueue::GetDepth() const
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      return pending_.size();
   }

   size_t
   RspamdLessonQueue::GetCapacity() const
   {
      return capacity_;
   }

   bool
   RspamdLessonQueue::GetDrainerRunning() const
   {
      boost::lock_guard<boost::mutex> guard(mutex_);
      return drainer_running_;
   }

   RspamdLessonQueue &
   RspamdLessonQueue::Instance()
   {
      // Never freed: a drainer task the asynchronous work queue frees as the
      // process ends still tells it it has ended.
      static RspamdLessonQueue *queue = new RspamdLessonQueue(RspamdLearner::QueueCapacity);
      return *queue;
   }
}
