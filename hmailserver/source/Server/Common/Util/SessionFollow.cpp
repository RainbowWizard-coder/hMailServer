// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "SessionFollow.h"
#include "Time.h"

#include <algorithm>
#include <chrono>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   SessionFollow::SessionFollow() :
      next_sequence_(1),
      waiters_(0),
      cancel_generation_(0),
      waits_allowed_(true)
   {
      ring_.resize(RingEntries);
   }

   SessionFollow::~SessionFollow()
   {
   }

   void
   SessionFollow::OnSessionStarted(int sessionId, const AnsiString &protocol, const AnsiString &peer, int localPort, bool client)
   {
      Session session;
      session.id = sessionId;
      session.protocol = protocol;
      session.peer = peer;
      session.local_port = localPort;
      session.client = client;
      session.open = true;
      session.started = Time::GetCurrentDateTime();

      std::lock_guard<std::mutex> guard(mutex_);
      open_[sessionId] = session;
   }

   void
   SessionFollow::OnSessionAuthenticated(int sessionId, const String &account)
   {
      std::lock_guard<std::mutex> guard(mutex_);

      std::map<int, Session>::iterator found = open_.find(sessionId);
      if (found != open_.end())
         found->second.account = account;
   }

   void
   SessionFollow::OnSessionEnded(int sessionId)
   {
      std::lock_guard<std::mutex> guard(mutex_);

      std::map<int, Session>::iterator found = open_.find(sessionId);
      if (found == open_.end())
         return;   // pre-created and never started, or started before this unit heard of it

      Session session = found->second;
      open_.erase(found);

      session.open = false;
      session.ended = Time::GetCurrentDateTime();

      ended_.push_front(session);
      while (ended_.size() > (size_t) EndedKept)
         ended_.pop_back();
   }

   void
   SessionFollow::OnLogEntry(const String &category, int session, const String &time, const String &message)
   {
      // The text is read for a session id before the lock, since it is the one
      // part of this that is not a copy.
      int named = session >= 0 ? session : NamedSession_(message);

      std::lock_guard<std::mutex> guard(mutex_);

      Line &slot = ring_[(size_t) ((next_sequence_ - 1) % RingEntries)];
      slot.sequence = next_sequence_++;
      slot.session = session;
      slot.named = named;
      slot.category = category;
      slot.time = time;

      if (message.GetLength() > MaxMessageChars)
         slot.message = message.Mid(0, MaxMessageChars) + _T(" [cut]");
      else
         slot.message = message;

      wake_.notify_all();
   }

   int
   SessionFollow::NamedSession_(const String &message)
   {
      // Almost no line says "session" at all, and this runs on the logging
      // thread for every entry with no session column - so the case-sensitive
      // search for the part of the word that neither spelling changes comes
      // first, and the copy and the lowering are paid only by the few lines
      // that could name one.
      if (message.Find(_T("ession")) < 0)
         return -1;

      // "TCP connection started for session 41", "Session Id: 41, Remote IP:",
      // "Ending session 41": the word, then optionally "id" and a colon, then
      // a space, then the number. The first one wins; a line naming two
      // sessions is filed under the first, which is the one the sentence is
      // about.
      //
      // The space in front of the number is required, and that is the whole
      // of what keeps a stranger's text out of an administrator's follow.
      // Plenty of lines with no session column quote what a remote party
      // chose - a request target, an envelope sender, a user name - and
      // without the requirement the token "session41" in any of them would
      // file that line under session 41 and put a sentence of somebody
      // else's writing in front of an administrator debugging session 41. A
      // space cannot be put in a request target, and every line this server
      // writes itself has one there.
      String lower = message;
      lower.ToLower();

      const String word = _T("session");
      int at = lower.Find(word);
      const int length = lower.GetLength();

      while (at >= 0)
      {
         int position = at + word.GetLength();

         while (position < length && lower[position] == ' ')
            position++;

         if (position + 1 < length && lower[position] == 'i' && lower[position + 1] == 'd')
         {
            position += 2;
            while (position < length && lower[position] == ' ')
               position++;
         }

         if (position < length && lower[position] == ':')
         {
            position++;
            while (position < length && lower[position] == ' ')
               position++;
         }

         if (position < length && lower[position - 1] == ' ' && lower[position] >= '0' && lower[position] <= '9')
         {
            long long value = 0;
            while (position < length && lower[position] >= '0' && lower[position] <= '9')
            {
               value = value * 10 + (lower[position] - '0');
               if (value > 0x7fffffff)
                  return -1;
               position++;
            }

            return (int) value;
         }

         at = lower.Find(word, at + word.GetLength());
      }

      return -1;
   }

   std::vector<SessionFollow::Session>
   SessionFollow::ListSessions(bool &truncated)
   {
      std::vector<Session> result;
      truncated = false;

      std::lock_guard<std::mutex> guard(mutex_);

      // Newest first: the map is ordered by id, and ids only ever grow.
      for (std::map<int, Session>::reverse_iterator iter = open_.rbegin(); iter != open_.rend(); ++iter)
      {
         if (result.size() >= (size_t) MaxListed)
         {
            truncated = true;
            break;
         }

         result.push_back(iter->second);
      }

      for (std::deque<Session>::const_iterator iter = ended_.begin(); iter != ended_.end(); ++iter)
         result.push_back(*iter);

      return result;
   }

   bool
   SessionFollow::FindSession(int sessionId, Session &out_session)
   {
      std::lock_guard<std::mutex> guard(mutex_);

      std::map<int, Session>::const_iterator found = open_.find(sessionId);
      if (found != open_.end())
      {
         out_session = found->second;
         return true;
      }

      for (std::deque<Session>::const_iterator iter = ended_.begin(); iter != ended_.end(); ++iter)
      {
         if (iter->id == sessionId)
         {
            out_session = *iter;
            return true;
         }
      }

      return false;
   }

   namespace
   {
      // The waiter budget is held by an object and not by a ++ and a --.
      // Follow copies strings while it scans (which can throw bad_alloc) and
      // waits on a condition variable (which can throw system_error): either
      // would leave the count raised for the life of the process, and since
      // nothing ever resets it, MaxWaiters leaks would mean no follow could
      // ever wait again on that process - the feature degrading silently to
      // a one-second poll for everybody, for good. Nor is it reset anywhere:
      // a reset that ran while a waiter was unwinding would take the count
      // below zero and lift the bound instead of restoring it.
      class WaiterSlot
      {
      public:
         explicit WaiterSlot(int &count) : count_(count), held_(false) {}
         ~WaiterSlot() { if (held_) count_--; }

         void Take() { count_++; held_ = true; }

      private:
         WaiterSlot(const WaiterSlot &);
         WaiterSlot &operator=(const WaiterSlot &);

         int &count_;
         bool held_;
      };
   }

   SessionFollow::FollowResult
   SessionFollow::Follow(int sessionId, long long after, int waitSeconds)
   {
      FollowResult result;

      if (waitSeconds < 0)
         waitSeconds = 0;
      if (waitSeconds > MaxWaitSeconds)
         waitSeconds = MaxWaitSeconds;

      std::unique_lock<std::mutex> lock(mutex_);

      // A listener that is stopping wakes its followers and then refuses the
      // next one a wait, so a follow that arrives in the instant after the
      // wake does not hold a worker thread while the listener is trying to
      // close. Read under the lock, with the flag CancelWaits sets.
      if (!waits_allowed_)
         waitSeconds = 0;

      // Constructed after the lock, so that its destructor - which touches
      // waiters_ - runs before the lock is given up, however this returns.
      WaiterSlot slot(waiters_);

      const unsigned long long generation = cancel_generation_;
      const std::chrono::steady_clock::time_point deadline =
         std::chrono::steady_clock::now() + std::chrono::seconds(waitSeconds);

      long long cursor = after < 0 ? 0 : after;

      for (;;)
      {
         // A cursor from before this process, or from a ring that has since
         // been emptied, is brought back into range rather than waited on
         // for lines that will never have a higher number.
         if (cursor >= next_sequence_)
            cursor = next_sequence_ - 1;

         const long long oldest = next_sequence_ > (long long) RingEntries ? next_sequence_ - RingEntries : 1;
         if (cursor + 1 < oldest)
         {
            if (cursor > 0)
               result.dropped = true;

            cursor = oldest - 1;
         }

         while (cursor + 1 < next_sequence_ && result.lines.size() < (size_t) MaxLinesPerAnswer)
         {
            cursor++;

            const Line &line = ring_[(size_t) ((cursor - 1) % RingEntries)];
            if (line.session == sessionId || line.named == sessionId)
               result.lines.push_back(line);
         }

         if (!result.lines.empty() || waitSeconds == 0)
            break;

         if (!result.waited)
         {
            if (waiters_ >= MaxWaiters)
               break;

            slot.Take();
            result.waited = true;
         }

         if (std::chrono::steady_clock::now() >= deadline)
            break;

         wake_.wait_until(lock, deadline);

         if (cancel_generation_ != generation)
            break;
      }

      result.cursor = cursor;
      return result;
   }

   void
   SessionFollow::CancelWaits()
   {
      std::lock_guard<std::mutex> guard(mutex_);
      waits_allowed_ = false;
      cancel_generation_++;
      wake_.notify_all();
   }

   void
   SessionFollow::AllowWaits()
   {
      std::lock_guard<std::mutex> guard(mutex_);
      waits_allowed_ = true;
   }

   long long
   SessionFollow::HeldEntries()
   {
      std::lock_guard<std::mutex> guard(mutex_);

      long long held = next_sequence_ - 1;
      if (held > (long long) RingEntries)
         held = RingEntries;

      return held;
   }
}
