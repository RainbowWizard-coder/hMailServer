// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <boost/thread/mutex.hpp>

#include <deque>
#include <vector>

namespace HM
{
   class Account;
   class Message;

   // RSPAMD AS THE BACKEND OF LEARNING FROM THE JUNK FOLDER (MoveLearning.h,
   // GitLab work item #108): each lesson is one request to Rspamd's controller,
   // as its protocol documents it - POST /learnspam or POST /learnham, the whole
   // message as stored (RFC 5322) as the body, and these headers:
   //
   //   Password    the controller's password (RspamdControllerPassword), when one
   //               is set. Learning is one of the controller's privileged
   //               commands: Rspamd checks it against enable_password, or
   //               against password when no enable_password is configured, and
   //               lets a request from one of its secure_ip addresses in without
   //               one. Never logged, never in an error text.
   //   Classifier  the classifier to teach (RspamdClassifier), which must be
   //               named: without the header Rspamd teaches every classifier it
   //               has, the global one among them, and a user's move would teach
   //               the whole server. A per-user classifier (per_user = true in
   //               its configuration) is the one to name.
   //   Deliver-To  the mailbox owner's full address: the user a per_user
   //               classifier keeps the statistics of, since Rspamd takes the
   //               principal recipient first from this header. Never the sender,
   //               nor anything in the message.
   //   Rcpt        the same address, as the request's one envelope recipient, so
   //               that a per_user setting that reads the recipients reads the
   //               owner and not the message's To: header.
   //
   // Rspamd answers {"success":true} with 200 when it learned the message. It
   // keeps, per user, which class it learned each message as (its learn cache),
   // and a message taught again as the same class is answered with an error
   // "<id> has been already learned as spam, ignore it" (208, or 404 from the
   // cache), which is logged as already learned and changes nothing; a message
   // taught as the other class - a correction, out of Junk after into it - is
   // unlearned from the first class and learned as the second, by Rspamd, in the
   // same request. 204 is a lesson Rspamd declined (its learn conditions, or a
   // message with too few or too many tokens for the classifier); 401 and 403 a
   // password refused; 404 a classifier Rspamd does not have ("class 'spam' is
   // not defined in classifier ..."), or an address that is not the controller;
   // each is logged, and
   // the two that an administrator must put right are reported (ErrorManager).
   // Nothing is retried.
   //
   // Off the IMAP thread entirely: a committed command queues its lessons here, in
   // a queue of at most QueueCapacity, and one task of the asynchronous work queue
   // sends them in order, one at a time, each bounded by RspamdTimeoutSeconds (the
   // name's lookup, the connection, TLS, sending and the answer together). A full
   // queue takes no more, and the command's newest lessons are dropped, said so in
   // the application log once for the command. Held in memory only: a restart of
   // the service loses what was waiting.
   //
   // The controller's URL is https - its certificate verified against the
   // machine's roots with the host name checked, as every HTTPS client of this
   // server verifies, with no switch to turn that off - or plain http to this
   // machine only (localhost, 127.0.0.0/8, ::1), since the password and the
   // message would otherwise cross a network in the clear. Never through
   // HttpProxy, and never to a link-local or cloud metadata address
   // (HttpsClient::RequestNotLinkLocal).
   class RspamdLearner
   {
   public:
      // The most lessons that wait at once; a few hundred bytes each.
      static const size_t QueueCapacity = 1000;

      // A message larger than Rspamd's own default max_message (50 MB) is not
      // sent: the controller would refuse it, after it had been read into memory.
      static const unsigned int MaxMessageBytes = 50 * 1024 * 1024;

      // The most of an answer that is read.
      static const unsigned int MaxAnswerBytes = 64 * 1024;

      // One lesson, as a committed command hands it on.
      class Lesson
      {
      public:
         Lesson() : spam(false) {}

         std::shared_ptr<Message> copy;
         std::shared_ptr<const Account> owner;
         bool spam;
      };

      // What a request came to.
      enum Outcome
      {
         OutcomeLearned = 0,
         OutcomeAlreadyLearned = 1,
         OutcomeDeclined = 2,
         OutcomePasswordRefused = 3,
         OutcomeNoClassifier = 4,
         OutcomeFailed = 5
      };

      // The request a lesson is: the URL and the headers HttpsClient is given
      // beside its own (Host, Content-Type, Content-Length, Connection).
      class Request
      {
      public:
         AnsiString url;
         std::vector<AnsiString> headers;
      };

      // The checks the settings are held to, each the sentence a refusal says,
      // naming the setting as settingName; empty when the value will do. An empty
      // URL, classifier and password are accepted here: the backend chosen with
      // either of the first two empty teaches nothing, and says so when a lesson
      // comes (HM8580, HM8581).
      static String UrlProblem(const String &url, const String &settingName);
      static String PasswordProblem(const String &password, const String &settingName);

      // PasswordProblem, and whether the store can hold the password protected:
      // what COM and REST ask before they write it. Not asked of a password read
      // back, which the store has already.
      static String PasswordStoreProblem(const String &password, const String &settingName);
      static String ClassifierProblem(const String &classifier, const String &settingName);
      static String TimeoutProblem(long seconds, const String &settingName);

      // The request for one lesson. False, with the reason, when the settings or
      // the owner's address cannot make one.
      static bool BuildRequest(const String &controllerUrl, const String &password, const String &classifier,
                               const String &owner, bool spam, Request &request, String &problem);

      // What the controller's answer means. detail is Rspamd's own error text,
      // made safe for a log line, or what was wrong with the answer.
      static Outcome Interpret(int status, const AnsiString &body, String &detail);

      // The words of an outcome, for the log.
      static String OutcomeText(Outcome outcome);

      // Sends one request with message as its body, bounded by timeoutSeconds,
      // and interprets the answer; detail says why when no answer came at all.
      static Outcome Send(const Request &request, const AnsiString &message, int timeoutSeconds, String &detail);

      // Queues a committed command's lessons, in order, for the drainer.
      static void Queue(const std::vector<Lesson> &lessons);

      // Teaches one lesson, on the calling thread (the drainer's): reads the
      // settings as they are now, the message as stored, and sends it.
      static void Teach(const Lesson &lesson);

      // Forgets which misconfigurations were reported, so that the next lesson
      // that meets one reports it again: called when a setting of the backend
      // changes, since what was wrong may be wrong another way now.
      static void ForgetReports();

   private:
      static void ReportMisconfiguration_(int code, const String &description);
   };

   // The lessons waiting, oldest first, and at most a capacity of them; one
   // drainer at a time sends them. Offer says when one has to be started, Take
   // ends it when nothing is left, and DrainerEnded lets one that stopped early -
   // the servers stopped, its task never run - be replaced by the next Offer.
   class RspamdLessonQueue
   {
   public:
      explicit RspamdLessonQueue(size_t capacity);

      // As many of lessons as there is room for, from the first; returns how many.
      size_t Offer(const std::vector<RspamdLearner::Lesson> &lessons, bool &startDrainer);
      bool Take(RspamdLearner::Lesson &lesson);
      void DrainerEnded();

      size_t GetDepth() const;
      size_t GetCapacity() const;
      bool GetDrainerRunning() const;

      static RspamdLessonQueue &Instance();

   private:
      mutable boost::mutex mutex_;
      std::deque<RspamdLearner::Lesson> pending_;
      size_t capacity_;
      bool drainer_running_;
   };
}
