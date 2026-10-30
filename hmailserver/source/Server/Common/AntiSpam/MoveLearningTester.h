// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace HM
{
   // Learning from the Junk folder (MoveLearning.h, RspamdLearner.h) held to
   // examples.
   //
   // TestRules, which needs no database, file or socket: which copies are
   // lessons and which way - into Junk spam, out of it ham, nothing between two
   // ordinary folders or two Junk folders, nothing for a public folder, between
   // two accounts, or without an owner - for each backend, with each switch off,
   // a COPY with MoveLearningFromCopy on and off, a delegate's move (a lesson for
   // SpamAssassin, as it always was, and none for Rspamd); which folder is Junk
   // when a "Junk" and a "Spam" folder are both there, and when "Spam" is
   // designated \Junk; the Rspamd request a lesson makes (the endpoint, the
   // Password, Classifier, Deliver-To and Rcpt headers, nothing else), the
   // settings it is refused for, and what each answer of the controller means;
   // and the lesson queue's bound - the newest dropped, one drainer at a time.
   //
   // TestStandIn: the request itself, sent by the server's own client to a
   // stand-in controller on a loopback port of its own that records it and
   // answers as Rspamd answers - learned, already learned, a password refused,
   // and one that never answers, which costs the timeout and no more.
   //
   // ClassTester::DoTests runs both on Windows. On Linux move-learning-tests runs
   // TestRules, in the source packages' builds too, and rspamd-learn-tests runs
   // TestStandIn, which those builds leave out because they refuse sockets.
   class MoveLearningTester
   {
   public:
      void TestRules();
      void TestStandIn();
   };
}
