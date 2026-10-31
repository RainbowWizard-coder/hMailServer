// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "MoveLearningTester.h"

#include "MoveLearning.h"
#include "RspamdLearner.h"

#include "../BO/IMAPFolder.h"
#include "../BO/IMAPFolders.h"
#include "../../IMAP/IMAPSpecialUse.h"
#include "../Util/Strings/Formatter.h"

#include <boost/asio.hpp>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      void Expect(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(std::string("MoveLearningTester: ") + what);
      }

      // A move by account 7 between two of its own folders, with the chosen
      // backend switched on.
      MoveLearning::Facts OwnMove(int backend)
      {
         MoveLearning::Facts facts;
         facts.backend = backend;
         facts.spamassassin_learn_on_move = true;
         facts.spamassassin_enabled = true;
         facts.is_copy = false;
         facts.learn_from_copy = true;
         facts.has_owner = true;
         facts.session_account_id = 7;
         facts.source_account_id = 7;
         facts.destination_account_id = 7;
         return facts;
      }

      MoveLearning::Facts IntoJunk(int backend)
      {
         MoveLearning::Facts facts = OwnMove(backend);
         facts.destination_junk = true;
         return facts;
      }

      bool HasHeader(const RspamdLearner::Request &request, const char *header)
      {
         for (const AnsiString &line : request.headers)
         {
            if (line == AnsiString(header))
               return true;
         }

         return false;
      }

      bool HasHeaderNamed(const RspamdLearner::Request &request, const char *name)
      {
         const AnsiString prefix = AnsiString(name) + ":";
         for (const AnsiString &line : request.headers)
         {
            if (line.StartsWith(prefix))
               return true;
         }

         return false;
      }

      std::vector<RspamdLearner::Lesson> Lessons(size_t count)
      {
         return std::vector<RspamdLearner::Lesson>(count);
      }

      std::shared_ptr<IMAPFolder> Folder(std::shared_ptr<IMAPFolders> tree, __int64 id, const String &name, int designation)
      {
         std::shared_ptr<IMAPFolder> folder = std::shared_ptr<IMAPFolder>(new IMAPFolder(7, -1));
         folder->SetID(id);
         folder->SetFolderName(name);
         folder->SetSpecialUseFlags(designation);
         tree->AddItem(folder);
         return folder;
      }

      bool IsJunk(const std::map<__int64, int> &designations, __int64 id)
      {
         auto found = designations.find(id);
         return found != designations.end() && (found->second & IMAPSpecialUse::DesignationJunk) != 0;
      }

      // A stand-in for Rspamd's controller on a loopback port of its own: takes one
      // connection, reads one request to the end of its body, records it, and
      // answers with answer - or, silent, answers nothing and waits for the client
      // to give up. Everything bounded: it stops by itself after limitSeconds.
      class StandIn
      {
      public:
         StandIn(const std::string &answer, bool silent, int limitSeconds) :
            acceptor_(io_, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0)),
            socket_(io_),
            answer_(answer),
            silent_(silent),
            limit_seconds_(limitSeconds)
         {
            port_ = acceptor_.local_endpoint().port();
         }

         // A block an expectation leaves by throwing, between Start and Join, ends
         // the stand-in here: a joinable std::thread destroyed unjoined would end
         // the process (std::terminate) - the hMailServer service itself, where
         // ClassTester runs - instead of the failure being reported as one.
         // io_context::stop may be called from any thread, and a run_for that has
         // not begun yet returns at once.
         ~StandIn()
         {
            io_.stop();
            Join();
         }

         StandIn(const StandIn &) = delete;
         StandIn &operator=(const StandIn &) = delete;

         unsigned short Port() const { return port_; }

         void Start()
         {
            thread_ = std::thread([this]()
            {
               acceptor_.async_accept(socket_, [this](const boost::system::error_code &error)
               {
                  if (!error)
                     Read_();
               });

               io_.run_for(std::chrono::seconds(limit_seconds_));

               Close_(socket_);
               Close_(acceptor_);
            });
         }

         void Join()
         {
            if (thread_.joinable())
               thread_.join();
         }

         const std::string &Received() const { return received_; }

      private:
         void Read_()
         {
            socket_.async_read_some(boost::asio::buffer(chunk_, sizeof(chunk_)),
               [this](const boost::system::error_code &error, std::size_t length)
            {
               if (length > 0)
                  received_.append(chunk_, length);

               if (error)
                  return;

               if (!silent_ && Complete_())
               {
                  boost::asio::async_write(socket_, boost::asio::buffer(answer_),
                     [this](const boost::system::error_code &, std::size_t)
                  {
                     boost::system::error_code error;
                     if (socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, error))
                        error.clear(); // a peer gone already: closed below all the same
                     Close_(socket_);
                     io_.stop();
                  });
                  return;
               }

               Read_();
            });
         }

         // Closes a socket or the acceptor the stand-in is done with. A close that
         // fails leaves nothing for the stand-in to do, so its error is let go
         // here, read, as the call returns it.
         template <class Closable>
         static void Close_(Closable &closable)
         {
            boost::system::error_code error;
            if (closable.close(error))
               error.clear();
         }

         bool Complete_() const
         {
            const size_t end = received_.find("\r\n\r\n");
            if (end == std::string::npos)
               return false;

            const std::string head = received_.substr(0, end);
            const std::string key = "\r\nContent-Length: ";
            const size_t at = head.find(key);
            if (at == std::string::npos)
               return true;

            const size_t length = (size_t) strtoul(head.c_str() + at + key.size(), nullptr, 10);
            return received_.size() >= end + 4 + length;
         }

         boost::asio::io_context io_;
         boost::asio::ip::tcp::acceptor acceptor_;
         boost::asio::ip::tcp::socket socket_;
         std::thread thread_;
         std::string answer_;
         std::string received_;
         bool silent_;
         int limit_seconds_;
         unsigned short port_;
         char chunk_[4096] = {};
      };

      std::string Answer(const char *statusLine, const std::string &body)
      {
         return std::string("HTTP/1.1 ") + statusLine + "\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string((unsigned long long) body.size()) + "\r\nConnection: close\r\n\r\n" + body;
      }

      bool Contains(const std::string &text, const std::string &part)
      {
         return text.find(part) != std::string::npos;
      }
   }

   void
   MoveLearningTester::TestRules()
   {
      bool spam = false;
      const int sa = MoveLearning::BackendSpamAssassin;
      const int rspamd = MoveLearning::BackendRspamd;

      // Each backend: into Junk is spam, out of it ham.
      for (int backend = sa; backend <= rspamd; backend++)
      {
         Expect(MoveLearning::IsLesson(IntoJunk(backend), spam) && spam, "a move into the Junk folder is a lesson of spam");

         MoveLearning::Facts facts = OwnMove(backend);
         facts.source_junk = true;
         Expect(MoveLearning::IsLesson(facts, spam) && !spam, "a move out of the Junk folder is a lesson of ham");

         facts = OwnMove(backend);
         Expect(!MoveLearning::IsLesson(facts, spam), "a move between two ordinary folders teaches nothing");

         facts = IntoJunk(backend);
         facts.source_junk = true;
         Expect(!MoveLearning::IsLesson(facts, spam), "a move between two Junk folders teaches nothing");

         facts = IntoJunk(backend);
         facts.destination_public = true;
         facts.destination_account_id = 0;
         Expect(!MoveLearning::IsLesson(facts, spam), "a move into a public folder teaches nothing");

         facts = OwnMove(backend);
         facts.source_junk = true;
         facts.source_public = true;
         facts.source_account_id = 0;
         Expect(!MoveLearning::IsLesson(facts, spam), "a move out of a public folder teaches nothing");

         facts = IntoJunk(backend);
         facts.destination_account_id = 8;
         Expect(!MoveLearning::IsLesson(facts, spam), "a move into another account's folder teaches nothing");

         facts = IntoJunk(backend);
         facts.has_owner = false;
         Expect(!MoveLearning::IsLesson(facts, spam), "a copy whose owner could not be read teaches nothing");

         // A COPY: a lesson as it always was, until MoveLearningFromCopy says no.
         facts = IntoJunk(backend);
         facts.is_copy = true;
         Expect(MoveLearning::IsLesson(facts, spam) && spam, "a COPY into Junk teaches while MoveLearningFromCopy is on");

         facts.learn_from_copy = false;
         Expect(!MoveLearning::IsLesson(facts, spam), "a COPY into Junk teaches nothing while MoveLearningFromCopy is off");

         facts = OwnMove(backend);
         facts.source_junk = true;
         facts.is_copy = true;
         facts.learn_from_copy = false;
         Expect(!MoveLearning::IsLesson(facts, spam), "a COPY out of Junk teaches nothing while MoveLearningFromCopy is off");

         facts = IntoJunk(backend);
         facts.learn_from_copy = false;
         Expect(MoveLearning::IsLesson(facts, spam) && spam, "a MOVE teaches whatever MoveLearningFromCopy says");
      }

      // SpamAssassin's switches, read as they always were.
      MoveLearning::Facts facts = IntoJunk(sa);
      facts.spamassassin_learn_on_move = false;
      Expect(!MoveLearning::IsLesson(facts, spam), "SpamAssassinLearnOnMove off teaches SpamAssassin nothing");

      facts = IntoJunk(sa);
      facts.spamassassin_enabled = false;
      Expect(!MoveLearning::IsLesson(facts, spam), "SpamAssassin off is taught nothing");

      facts = IntoJunk(rspamd);
      facts.spamassassin_learn_on_move = false;
      facts.spamassassin_enabled = false;
      Expect(MoveLearning::IsLesson(facts, spam) && spam, "Rspamd chosen teaches whatever the SpamAssassin switches say");

      facts = IntoJunk(5);
      Expect(!MoveLearning::IsLesson(facts, spam), "a backend that is neither teaches nothing");

      // Whose lesson: a delegate's move in the owner's folders.
      facts = IntoJunk(sa);
      facts.session_account_id = 8;
      Expect(MoveLearning::IsLesson(facts, spam) && spam, "for SpamAssassin a delegate's move into the owner's Junk is the owner's lesson, as it always was");

      facts = IntoJunk(rspamd);
      facts.session_account_id = 8;
      Expect(!MoveLearning::IsLesson(facts, spam), "for Rspamd a delegate's move in another account's folders teaches nobody");

      facts = IntoJunk(rspamd);
      facts.session_account_id = 0;
      Expect(!MoveLearning::IsLesson(facts, spam), "for Rspamd a move with no account signed in teaches nobody");

      Expect(MoveLearning::MayTeach(OwnMove(rspamd)), "MayTeach holds for an own move before the Junk folder is looked for");

      // Which folder is Junk: by name, the first of Junk, Junk E-mail, Junk Email
      // and Spam at the top level; an explicit \Junk wins over every name.
      {
         std::shared_ptr<IMAPFolders> tree = std::shared_ptr<IMAPFolders>(new IMAPFolders(7, -1));
         Folder(tree, 1, _T("INBOX"), 0);
         Folder(tree, 2, _T("Junk"), 0);
         Folder(tree, 3, _T("Spam"), 0);

         std::map<__int64, int> designations;
         IMAPSpecialUse::Resolve(tree, designations);
         Expect(IsJunk(designations, 2) && !IsJunk(designations, 3) && !IsJunk(designations, 1),
            "with a Junk and a Spam folder and neither designated, Junk is the Junk folder and Spam an ordinary one");
      }

      {
         std::shared_ptr<IMAPFolders> tree = std::shared_ptr<IMAPFolders>(new IMAPFolders(7, -1));
         Folder(tree, 1, _T("INBOX"), 0);
         Folder(tree, 2, _T("Junk"), 0);
         Folder(tree, 3, _T("Spam"), IMAPSpecialUse::DesignationJunk);

         std::map<__int64, int> designations;
         IMAPSpecialUse::Resolve(tree, designations);
         Expect(!IsJunk(designations, 2) && IsJunk(designations, 3),
            "a Spam folder designated \\Junk is the Junk folder, and Junk beside it an ordinary one");
      }

      {
         std::shared_ptr<IMAPFolders> tree = std::shared_ptr<IMAPFolders>(new IMAPFolders(7, -1));
         Folder(tree, 1, _T("INBOX"), 0);
         Folder(tree, 3, _T("Spam"), 0);

         std::map<__int64, int> designations;
         IMAPSpecialUse::Resolve(tree, designations);
         Expect(IsJunk(designations, 3), "a Spam folder alone is the Junk folder");
      }

      // The request a lesson makes.
      RspamdLearner::Request request;
      String problem;
      Expect(RspamdLearner::BuildRequest(_T("http://127.0.0.1:11334"), _T("s3cret!"), _T("bayes_user"), _T("owner@example.test"), true, request, problem),
         "a lesson to the controller on this machine makes a request");
      Expect(request.url == "http://127.0.0.1:11334/learnspam", "spam goes to /learnspam");
      Expect(request.headers.size() == 4 && HasHeader(request, "Password: s3cret!") && HasHeader(request, "Classifier: bayes_user") &&
             HasHeader(request, "Deliver-To: owner@example.test") && HasHeader(request, "Rcpt: owner@example.test"),
         "the request carries the password, the classifier and the owner as Deliver-To and Rcpt, and nothing else");

      Expect(RspamdLearner::BuildRequest(_T("https://rspamd.example.test/controller/"), _T(""), _T("bayes_user"), _T("owner@example.test"), false, request, problem),
         "an https controller under a path makes a request");
      Expect(request.url == "https://rspamd.example.test/controller/learnham", "ham goes to /learnham, after the controller's path");
      Expect(!HasHeaderNamed(request, "Password"), "no password set, no Password header");

      Expect(!RspamdLearner::BuildRequest(_T(""), _T(""), _T("bayes_user"), _T("owner@example.test"), true, request, problem), "no URL, no request");
      Expect(!RspamdLearner::BuildRequest(_T("http://rspamd.example.test:11334"), _T(""), _T("bayes_user"), _T("owner@example.test"), true, request, problem),
         "plain http to another machine is refused");
      Expect(!RspamdLearner::BuildRequest(_T("http://127.0.0.1:11334"), _T(""), _T(""), _T("owner@example.test"), true, request, problem),
         "no classifier, no request: Rspamd would teach every classifier, the global one among them");
      Expect(!RspamdLearner::BuildRequest(_T("http://127.0.0.1:11334"), _T(""), _T("bayes_user"), _T("owner@example.test\r\nClassifier: bayes"), true, request, problem),
         "an owner's address that would end a header makes no request");
      Expect(!RspamdLearner::BuildRequest(_T("http://127.0.0.1:11334"), _T("two words"), _T("bayes_user"), _T("owner@example.test"), true, request, problem),
         "a password with a space makes no request");

      Expect(RspamdLearner::UrlProblem(_T("http://127.0.0.1:11334/?password=x"), _T("u")).GetLength() > 0, "a URL with a query is refused");
      Expect(RspamdLearner::UrlProblem(_T("https://user:pw@rspamd.example.test"), _T("u")).GetLength() > 0, "a URL with a password in it is refused");
      Expect(RspamdLearner::UrlProblem(_T("ftp://rspamd.example.test"), _T("u")).GetLength() > 0, "a URL that is not http or https is refused");
      Expect(RspamdLearner::UrlProblem(_T("http://[::1]:11334"), _T("u")).IsEmpty(), "plain http to ::1 is accepted");
      Expect(RspamdLearner::UrlProblem(_T("http://localhost:11334"), _T("u")).IsEmpty(), "plain http to localhost is accepted");
      Expect(RspamdLearner::TimeoutProblem(0, _T("t")).GetLength() > 0 && RspamdLearner::TimeoutProblem(301, _T("t")).GetLength() > 0 &&
             RspamdLearner::TimeoutProblem(15, _T("t")).IsEmpty(), "the timeout is 1 to 300 seconds");
      Expect(RspamdLearner::ClassifierProblem(_T("bayes user"), _T("c")).GetLength() > 0, "a classifier's name has no space");

      // What the controller's answers mean.
      String detail;
      Expect(RspamdLearner::Interpret(200, "{\"success\":true}", detail) == RspamdLearner::OutcomeLearned, "200 with success true is learned");
      Expect(RspamdLearner::Interpret(200, "{\"success\": true }", detail) == RspamdLearner::OutcomeLearned, "and with spaces in it");
      Expect(RspamdLearner::Interpret(200, "{}", detail) == RspamdLearner::OutcomeFailed, "200 without success true is not learned");
      Expect(RspamdLearner::Interpret(208, "{\"error\":\"<abc@example.test> has been already learned as spam, ignore it\"}", detail) == RspamdLearner::OutcomeAlreadyLearned &&
             detail.Find(_T("already learned")) >= 0, "208 is already learned, with Rspamd's words");
      Expect(RspamdLearner::Interpret(404, "{\"error\":\"<abc@example.test> has been already learned as ham, ignore it\"}", detail) == RspamdLearner::OutcomeAlreadyLearned,
         "the learn cache's 404 is already learned too");
      Expect(RspamdLearner::Interpret(404, "{\"error\":\"cannot find classifier with name bayes_users\"}", detail) == RspamdLearner::OutcomeNoClassifier,
         "a classifier Rspamd does not have");
      Expect(RspamdLearner::Interpret(404, "{\"error\":\"class 'spam' is not defined in classifier bayes_nobody\"}", detail) == RspamdLearner::OutcomeNoClassifier &&
             detail.Find(_T("bayes_nobody")) >= 0, "a classifier Rspamd 4.2 does not have, in its words");
      Expect(RspamdLearner::Interpret(204, "", detail) == RspamdLearner::OutcomeDeclined, "204 is a lesson Rspamd declined");
      Expect(RspamdLearner::Interpret(401, "{\"error\":\"Unauthorized\"}", detail) == RspamdLearner::OutcomePasswordRefused, "401 is the password refused");
      Expect(RspamdLearner::Interpret(403, "", detail) == RspamdLearner::OutcomePasswordRefused, "403 is the password refused");
      Expect(RspamdLearner::Interpret(500, "{\"error\":\"Cannot push learned results to the backend\"}", detail) == RspamdLearner::OutcomeFailed &&
             detail.Find(_T("500")) >= 0, "500 is not learned, with the code");
      Expect(RspamdLearner::Interpret(500, "{\"error\":\"line\\r\\nbreak\"}", detail) == RspamdLearner::OutcomeFailed && detail.Find(_T("\r")) < 0,
         "Rspamd's words reach the log on one line");

      // The queue's bound: the newest dropped, one drainer at a time.
      RspamdLessonQueue queue(3);
      bool start = false;
      Expect(queue.Offer(Lessons(2), start) == 2 && start, "the first lessons are taken, and a drainer is to be started");
      Expect(queue.Offer(Lessons(2), start) == 1 && !start, "past the bound the newest are dropped, and no second drainer is started");
      Expect(queue.GetDepth() == 3 && queue.GetDrainerRunning(), "the queue holds its bound, with its drainer counted");
      RspamdLearner::Lesson taken;
      Expect(queue.Take(taken) && queue.Take(taken) && queue.Take(taken), "the drainer takes them all");
      Expect(!queue.Take(taken) && !queue.GetDrainerRunning(), "an empty queue ends its drainer");
      Expect(queue.Offer(Lessons(1), start) == 1 && start, "and the next offer starts another");
      queue.DrainerEnded();
      Expect(queue.GetDepth() == 1 && !queue.GetDrainerRunning(), "a drainer that stopped early leaves what is waiting");
      Expect(RspamdLessonQueue::Instance().GetCapacity() == RspamdLearner::QueueCapacity, "the server's queue holds QueueCapacity lessons");
   }

   void
   MoveLearningTester::TestStandIn()
   {
      const AnsiString message = "From: sender@example.test\r\nTo: someone-else@example.test\r\nSubject: Lesson\r\n\r\nBody text.\r\n";

      // Learned: the request as Rspamd's protocol has it, the message as its body.
      {
         StandIn controller(Answer("200 OK", "{\"success\":true}"), false, 15);
         controller.Start();

         RspamdLearner::Request request;
         String problem;
         const String url = Formatter::Format(_T("http://127.0.0.1:{0}"), (int) controller.Port());
         Expect(RspamdLearner::BuildRequest(url, _T("s3cret!"), _T("bayes_user"), _T("owner@example.test"), true, request, problem), "the request is made");

         String detail;
         const RspamdLearner::Outcome outcome = RspamdLearner::Send(request, message, 10, detail);
         controller.Join();

         const std::string &received = controller.Received();
         Expect(outcome == RspamdLearner::OutcomeLearned, "the stand-in's success is learned");
         Expect(received.compare(0, 25, "POST /learnspam HTTP/1.0\r") == 0, "the request line is POST /learnspam");
         Expect(Contains(received, "\r\nPassword: s3cret!\r\n") && Contains(received, "\r\nClassifier: bayes_user\r\n") &&
                Contains(received, "\r\nDeliver-To: owner@example.test\r\n") && Contains(received, "\r\nRcpt: owner@example.test\r\n"),
            "the stand-in is sent the password, the classifier, and the owner as Deliver-To and Rcpt");
         Expect(Contains(received, "\r\nContent-Type: message/rfc822\r\n") &&
                Contains(received, "\r\nContent-Length: " + std::to_string((unsigned long long) message.GetLength()) + "\r\n"),
            "the body is the message, as message/rfc822, its length said");
         Expect(received.size() >= (size_t) message.GetLength() &&
                received.compare(received.size() - message.GetLength(), message.GetLength(), message.c_str()) == 0,
            "the body is the whole message as stored, byte for byte");
         const std::string head = received.substr(0, received.find("\r\n\r\n") + 2);
         Expect(!Contains(head, "\r\nUser:") && !Contains(head, "\r\nFrom:"), "no other identity is sent: no User, no From header in the request");
      }

      // Ham, already learned as ham: logged as such, nothing changed.
      {
         StandIn controller(Answer("208 Already Reported", "{\"error\":\"<x@example.test> has been already learned as ham, ignore it\"}"), false, 15);
         controller.Start();

         RspamdLearner::Request request;
         String problem;
         const String url = Formatter::Format(_T("http://127.0.0.1:{0}/"), (int) controller.Port());
         Expect(RspamdLearner::BuildRequest(url, _T(""), _T("bayes_user"), _T("owner@example.test"), false, request, problem), "the ham request is made");

         String detail;
         const RspamdLearner::Outcome outcome = RspamdLearner::Send(request, message, 10, detail);
         controller.Join();

         Expect(controller.Received().compare(0, 24, "POST /learnham HTTP/1.0\r") == 0, "the request line is POST /learnham");
         Expect(!Contains(controller.Received(), "\r\nPassword:"), "no password set, none sent");
         Expect(outcome == RspamdLearner::OutcomeAlreadyLearned, "already learned is said so");
      }

      // A password refused: the outcome says so, and nothing says the password.
      {
         StandIn controller(Answer("401 Unauthorized", "{\"error\":\"Unauthorized\"}"), false, 15);
         controller.Start();

         RspamdLearner::Request request;
         String problem;
         const String url = Formatter::Format(_T("http://127.0.0.1:{0}"), (int) controller.Port());
         Expect(RspamdLearner::BuildRequest(url, _T("wr0ng-password"), _T("bayes_user"), _T("owner@example.test"), true, request, problem), "the request is made");

         String detail;
         const RspamdLearner::Outcome outcome = RspamdLearner::Send(request, message, 10, detail);
         controller.Join();

         Expect(outcome == RspamdLearner::OutcomePasswordRefused, "401 is the password refused");
         Expect(detail.Find(_T("wr0ng")) < 0, "the password is in no outcome text");
      }

      // A controller that takes the connection and never answers costs the timeout
      // and no more.
      {
         StandIn controller(std::string(), true, 15);
         controller.Start();

         RspamdLearner::Request request;
         String problem;
         const String url = Formatter::Format(_T("http://127.0.0.1:{0}"), (int) controller.Port());
         Expect(RspamdLearner::BuildRequest(url, _T(""), _T("bayes_user"), _T("owner@example.test"), true, request, problem), "the request is made");

         const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
         String detail;
         const RspamdLearner::Outcome outcome = RspamdLearner::Send(request, message, 2, detail);
         const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
         controller.Join();

         Expect(outcome == RspamdLearner::OutcomeFailed, "no answer is not learned");
         Expect(seconds >= 1.5 && seconds < 8.0, "no answer costs the two seconds' timeout, and no more");
         Expect(Contains(controller.Received(), "POST /learnspam"), "the silent stand-in was sent the request");
      }
   }
}
