// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Follow one connection live: the sessions the server has open, and every
// line the log carries for one of them, handed to a follower as it is written.
//
// WHAT IT IS FOR. The way a conversation is debugged today is to note the
// session id from one log line and grep the day's log for it - which on a
// busy server is a gigabyte, and which is only possible once the conversation
// is over. This unit keeps the last few thousand log entries in memory, with
// the session each belongs to, and keeps a list of the sessions that are open
// and the last ones that ended, so that an administrator can pick a session
// in the Deck and watch its lines appear.
//
// WHAT IT COSTS, and why it is always on. Every entry that reaches
// Logger::Write_ is copied into a ring of RingEntries slots under one mutex,
// so the cost is one lock and one string copy per log line the log mask lets
// through, and the memory is the ring: RingEntries lines of at most
// MaxMessageChars characters. There is no setting, because a ring that is
// filled only while somebody is watching has nothing to show for the seconds
// before they pressed Follow - and an SMTP session lasts less than that.
//
// WHICH LINES ARE A SESSION'S. The conversation lines (SMTPD, SMTPC, POP3D,
// POP3C, IMAPD) carry the session id in their own column. The DEBUG and TCPIP
// lines carry none, and name the session in their text instead - "TCP
// connection started for session 41", "Session Id: 41" - so an entry with no
// session column is read once, when it arrives, for the first number that
// follows the word "session", and is filed under that. What is not caught by
// either is not a session's line and is not shown; the document says so.
//
// WHAT A FOLLOW IS. Not a subscription: a cursor. Every entry has a sequence
// number, and a follower asks for the entries of one session after the last
// sequence it saw. When there are none yet it may wait - up to MaxWaitSeconds
// - for the next entry to arrive, which is what makes the lines appear as
// they happen rather than on a timer. The wait is bounded in number as well
// as in time, because a waiting follower is a thread of the HTTP listener's
// small pool: past MaxWaiters, a follow answers at once and the page polls.
// That pool is not the Deck's: the same four threads serve the webmail of
// every mailbox on this server, so the budget is deliberately one thread.
//
// It never logs. A line written from inside this unit would arrive back at
// OnLogEntry with the mutex held.

#pragma once

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace HM
{
   class SessionFollow : public Singleton<SessionFollow>
   {
   public:
      enum Constants
      {
         // Entries held. At the default MaxLogLineLen the ring is a few
         // megabytes; a debug-logged IMAP client fills it in a few minutes,
         // which is why a follow reports when it has fallen behind.
         RingEntries = 4096,

         // One entry's message is cut here. The file device cuts at
         // MaxLogLineLen (500 by default) on the way to disk; this is looser,
         // and exists so that one pathological line cannot hold a megabyte.
         MaxMessageChars = 2048,

         // Sessions kept after they end, so that a conversation that lasted a
         // quarter of a second can still be picked from the list and read.
         EndedKept = 200,

         // Open sessions a listing answers, newest first.
         MaxListed = 1000,

         // Lines one answer carries. A follower that is far behind catches up
         // over several answers rather than in one large one.
         MaxLinesPerAnswer = 500,

         // The longest one follow waits for a line. The HTTP listener's
         // request deadline is 30 seconds and counts the handler.
         MaxWaitSeconds = 20,

         // Follows that may wait at once: one. Each holds one of the REST
         // listener's four worker threads for the length of its wait
         // (WorkerThreads in RestApiServer.cpp), and those four are not the
         // Deck's own - the same handler answers /portal and every
         // /api/v1/me route, which is the webmail of every mailbox on this
         // server. A second waiting follow would be half the pool held by a
         // page somebody left open, and the route asks for nothing stronger
         // than view_logs, which the read-only auditor role and a read-only
         // API key both have. So the budget is a quarter of the pool, and
         // the second follower polls: it is answered at once with waited
         // false and asks again a second later, which costs it a second of
         // latency and costs the mail nothing.
         MaxWaiters = 1
      };

      struct Session
      {
         Session() : id(0), local_port(0), client(false), open(false) {}

         int id;
         AnsiString protocol;     // "SMTP", "POP3", "IMAP", "SMTP client", ...
         AnsiString peer;         // the effective remote address
         int local_port;
         bool client;             // this server opened the connection
         bool open;
         String account;          // once signed in; empty until then
         String started;
         String ended;            // empty while open
      };

      struct Line
      {
         Line() : sequence(0), session(-1), named(-1) {}

         long long sequence;
         int session;             // the entry's own session column, -1 for none
         int named;               // the session the text names, when the column is empty
         String category;
         String time;
         String message;
      };

      struct FollowResult
      {
         FollowResult() : cursor(0), dropped(false), waited(false) {}

         std::vector<Line> lines;

         // The sequence to ask after next time.
         long long cursor;

         // The follower's cursor pointed before the oldest entry held: lines
         // between were written and are gone.
         bool dropped;

         // This answer waited for a line rather than answering at once.
         bool waited;
      };

      SessionFollow();
      ~SessionFollow();

      // From TCPConnection, when the session starts, signs in, and ends.
      void OnSessionStarted(int sessionId, const AnsiString &protocol, const AnsiString &peer, int localPort, bool client);
      void OnSessionAuthenticated(int sessionId, const String &account);
      void OnSessionEnded(int sessionId);

      // From Logger::Write_, on whichever thread logged: every entry.
      void OnLogEntry(const String &category, int session, const String &time, const String &message);

      // The open sessions, newest first, then the ended ones, newest first.
      // truncated is set when there were more open sessions than MaxListed.
      std::vector<Session> ListSessions(bool &truncated);
      bool FindSession(int sessionId, Session &out_session);

      // The lines of one session after `after` (0 for everything held),
      // waiting up to waitSeconds for one when there is none yet.
      FollowResult Follow(int sessionId, long long after, int waitSeconds);

      // Wakes every waiting Follow, which answers with what it has, and stops
      // the next one waiting at all. From the REST listener's Stop, so a stop
      // never waits on a follower's timer - including a follow that arrived in
      // the instant between the wake and the listener closing, which a wake
      // alone would leave waiting out its twenty seconds. AllowWaits, from the
      // listener's Start, lets them wait again.
      void CancelWaits();
      void AllowWaits();

      // How many entries the ring holds now, for the listing's own words.
      long long HeldEntries();

   private:
      static int NamedSession_(const String &message);

      std::mutex mutex_;
      std::condition_variable wake_;

      std::vector<Line> ring_;
      long long next_sequence_;

      std::map<int, Session> open_;
      std::deque<Session> ended_;

      int waiters_;
      unsigned long long cancel_generation_;
      bool waits_allowed_;
   };
}
