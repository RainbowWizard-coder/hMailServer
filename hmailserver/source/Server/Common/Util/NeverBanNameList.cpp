// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See NeverBanNameList.h.

#include "StdAfx.h"

#include "NeverBanNameList.h"

#include "../BO/NeverBanName.h"
#include "../BO/NeverBanNames.h"
#include "../BO/SecurityRange.h"
#include "../BO/SecurityRanges.h"
#include "../Persistence/PersistenceMode.h"
#include "../Persistence/PersistentNeverBanName.h"
#include "../Persistence/PersistentSecurityRange.h"
#include "../TCPIP/DNSResolver.h"
#include "../TCPIP/IPAddress.h"
#include "../Threading/Task.h"
#include "../Threading/WorkQueue.h"
#include "../Threading/WorkQueueManager.h"
#include "Parsing/StringParser.h"
#include "Time.h"
#include "Unicode.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

#include <boost/thread/thread.hpp>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // ---- the rows
      //
      // The patterns as last read, normalised. A handful an administrator typed,
      // walked whole for every decision.
      std::mutex rows_mutex;
      bool need_refresh = true;
      std::vector<String> row_patterns;
      time_t last_read = 0;

      // A row another node added to a shared database is in force here after at
      // most this long; a row added here is in force at once.
      const int RereadSeconds = 300;

      // A table that cannot be read is retried after a minute and reported at
      // most once an hour, as DomainPolicyList does: the ban path asks for the
      // rows, and a database that is refusing must not have every ban queue
      // behind one more failing query, or write an error line each time.
      time_t last_refresh_failure = 0;
      time_t last_refresh_report = 0;

      // ---- the verdicts
      //
      // Lock order: rows_mutex before verdict_mutex, never the other way round.
      std::mutex verdict_mutex;

      struct Kept
      {
         NeverBanNameList::Verdict verdict;
         time_t expires = 0;
         bool used = false;
      };

      std::map<String, Kept> kept;

      // Bumped whenever the rows change, so that a verdict reached under rows
      // that are no longer the list is never kept and never acted on.
      std::atomic<unsigned> generation(0);

      const int KeepSparedSeconds = 3600;
      const int KeepRefusedSeconds = 600;

      // Every SMTP connection from an address the list has rows for leaves a
      // verdict, so the map is held to a size: expired entries go first, and if
      // that is not enough, everything goes - a verdict that is forgotten is
      // only asked again.
      const size_t MaxKept = 4096;

      // ---- the lookups of the waiting bans
      //
      // Each is made on a thread of its own and waited for until its deadline
      // (WithinDeadline below), whatever the resolver's own timeout - the
      // DNSQueryTimeout setting, which may be 0, no bound at all. A lookup
      // given up keeps its thread until the resolver gives up; this many such
      // threads at most, and past them a lookup is not begun and counts as not
      // answered, so that a resolver that never answers costs threads that are
      // counted.
      std::atomic<int> abandoned_lookups(0);

      // lookup made on a thread of its own and waited for until milliseconds
      // have passed: what it found, and whether it was answered; false at the
      // deadline, the thread left to finish on its own and counted in
      // abandoned until it does. lookup is copied to that thread, with
      // whatever it captured: nothing of this frame's. A thread of a work
      // queue that is asked to stop - the service stopping - stops waiting
      // within a tenth of a second, as at the deadline: the wait is in slices,
      // and a std::condition_variable is not a point boost's interruption
      // reaches.
      bool WithinDeadline(const std::function<bool(std::vector<String> &found)> &lookup, std::vector<String> &found, int milliseconds,
                          std::atomic<int> &abandoned = abandoned_lookups)
      {
         struct Shared
         {
            std::mutex lock;
            std::condition_variable finished;
            bool done = false;
            bool abandoned = false;
            bool answered = false;
            std::vector<String> values;
         };

         if (abandoned.load() >= NeverBanNameList::MaxAbandonedLookups)
            return false;

         std::shared_ptr<Shared> shared = std::make_shared<Shared>();
         std::atomic<int> *counter = &abandoned;

         try
         {
            std::thread([shared, lookup, counter]()
            {
               // The top of a thread: nothing may leave it.
               std::vector<String> values;
               bool answered = false;

               try
               {
                  answered = lookup(values);
               }
               catch (...)
               {
                  answered = false;
               }

               std::lock_guard<std::mutex> guard(shared->lock);
               shared->answered = answered;
               shared->values.swap(values);
               shared->done = true;

               if (shared->abandoned)
                  (*counter)--;

               shared->finished.notify_all();
            }).detach();
         }
         catch (...)
         {
            // No thread to be had: not begun, and not answered.
            return false;
         }

         const std::chrono::steady_clock::time_point until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
         const std::chrono::milliseconds slice(100);

         std::unique_lock<std::mutex> guard(shared->lock);
         while (!shared->done)
         {
            const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            if (now >= until || boost::this_thread::interruption_requested())
            {
               shared->abandoned = true;
               (*counter)++;
               return false;
            }

            const std::chrono::steady_clock::duration left = until - now;
            shared->finished.wait_for(guard, left < slice ? left : std::chrono::steady_clock::duration(slice));
         }

         found = shared->values;
         return shared->answered;
      }

      const wchar_t *RangeNamePrefix = L"Auto-ban: ";

      boost::asio::ip::address Canonical(const IPAddress &address)
      {
         boost::asio::ip::address value = address.GetAddress();

         if (value.is_v6() && value.to_v6().is_v4_mapped())
            return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, value.to_v6());

         return value;
      }

      String CanonicalText(const IPAddress &address)
      {
         return String(Canonical(address).to_string().c_str());
      }

      AnsiString JsonText(const String &value)
      {
         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);

         AnsiString out;

         for (int i = 0; i < utf8.GetLength(); i++)
         {
            const unsigned char character = (unsigned char) utf8[i];
            char single[2] = { (char) character, 0 };

            switch (character)
            {
            case '"':
               out += "\\\"";
               break;
            case '\\':
               out += "\\\\";
               break;
            case '\n':
               out += "\\n";
               break;
            case '\r':
               out += "\\r";
               break;
            case '\t':
               out += "\\t";
               break;
            default:
               if (character < 0x20)
               {
                  AnsiString escaped;
                  escaped.Format("\\u%04x", (int) character);
                  out += escaped;
               }
               else
               {
                  out += single;
               }
               break;
            }
         }

         return out;
      }

      AnsiString JsonQuoted(const String &value)
      {
         return "\"" + JsonText(value) + "\"";
      }

      const char *JsonFlag(bool value)
      {
         return value ? "true" : "false";
      }

      // A name from somebody's reverse zone, fit for a log line: printable ASCII
      // and no quote, at most 255 characters. The reverse zone's holder chooses
      // these bytes.
      String Printable(const String &name)
      {
         String out;
         const int length = name.GetLength() > 255 ? 255 : name.GetLength();

         for (int i = 0; i < length; i++)
         {
            const wchar_t c = name.GetAt(i);
            out += (c < 0x21 || c > 0x7E || c == L'"' || c == L'\\') ? L'?' : c;
         }

         return out;
      }
   }

   // Work handed to the verification queue: a waiting ban's thread (Drain_),
   // or the lift of a ban known to be spared. The queue's threads are boost's:
   // a stop interrupts them, and Drain_ and WithinDeadline see it.
   class NeverBanTask : public Task
   {
   public:
      explicit NeverBanTask(const std::function<void()> &work) :
         Task("NeverBanNameList::VerifyBan"),
         work_(work)
      {
      }

      virtual void DoWork()
      {
         try
         {
            work_();
         }
         catch (const boost::thread_interrupted &)
         {
            // The queue is stopping. Passed on, as WorkQueue asks: swallowed,
            // the request to stop would be spent and the worker would not know
            // it had been asked.
            throw;
         }
         catch (...)
         {
            // What the work was for waits as it was; the expiry pass takes it
            // up again.
            LOG_DEBUG(_T("NeverBanNameList - a verification ended in an error; what it was for waits for the next pass."));
         }
      }

   private:
      std::function<void()> work_;
   };

   // The bans waiting for the list's answer (NeverBanNameList.h says why they
   // wait, and how they are taken). One for the service, Global(); the tester
   // makes its own, with stand-ins for the world around it.
   //
   // Lock order: lock_ is never held while the rows, the verdicts, the ranges
   // or an announcement are reached - each of those is called with it released
   // - and AutoBanFirewall's reconciliation, which holds its own lock, takes
   // lock_ (IsPending) and nothing after it.
   class NeverBanWaitingBans
   {
   public:
      struct Surroundings
      {
         // The list's rows: false when it has none.
         std::function<bool(std::vector<String> &patterns)> rows;

         // A verdict already known about the address; never looks anything up.
         std::function<bool(const IPAddress &address, NeverBanNameList::Verdict &verdict)> known;

         // The address resolved, each lookup held to its deadline; the verdict
         // kept when the rows are still those it began under; and those rows.
         std::function<NeverBanNameList::Verdict(const IPAddress &address)> resolve;
         std::function<void(const IPAddress &address, const NeverBanNameList::Verdict &verdict, unsigned started)> keep;
         std::function<unsigned()> generation;

         // The ranges written for a spared address deleted: true when one was.
         std::function<bool(const IPAddress &address, const NeverBanNameList::Verdict &verdict)> lift;

         // The addresses, canonical, that have a single-address auto-ban range
         // in force: false when the ranges could not be read.
         std::function<bool(std::set<String> &addresses)> live;

         // work handed to a thread of the verification queue: false when
         // there is none to hand it to.
         std::function<bool(const std::function<void()> &work)> post;

         std::function<time_t()> now;
      };

      explicit NeverBanWaitingBans(const Surroundings &surroundings) :
         s_(surroundings)
      {
      }

      static NeverBanWaitingBans &Global()
      {
         static NeverBanWaitingBans bans(Real_());
         return bans;
      }

      void Hold(const IPAddress &address)
      {
         std::vector<String> patterns;
         if (!s_.rows(patterns))
            return;

         NeverBanNameList::Verdict known;
         if (s_.known(address, known) && !known.spared)
            return;

         std::function<void()> evicted;

         {
            std::lock_guard<std::mutex> guard(lock_);
            Admit_(NeverBanNameList::Key_(address), address, evicted);
         }

         Run_(evicted);
      }

      void Release(const IPAddress &address)
      {
         std::lock_guard<std::mutex> guard(lock_);

         // Only a hold no ban has followed: one VerifyBan has filled is decided
         // by its answer.
         auto it = waiting_.find(NeverBanNameList::Key_(address));
         if (it != waiting_.end() && !it->second.announce && !it->second.taken)
            waiting_.erase(it);
      }

      void VerifyBan(const IPAddress &address, const std::function<void()> &announce)
      {
         const String key = NeverBanNameList::Key_(address);

         std::vector<String> patterns;
         NeverBanNameList::Verdict known;
         const bool rows = s_.rows(patterns);
         const bool isKnown = rows && s_.known(address, known);

         if (!rows || (isKnown && !known.spared))
         {
            // Announced now: no list, or an answer already in that does not
            // spare it - and with it any earlier ban of the address still
            // waiting, which the same answer decides.
            std::function<void()> earlier;

            {
               std::lock_guard<std::mutex> guard(lock_);
               auto it = waiting_.find(key);
               if (it != waiting_.end() && !it->second.taken)
               {
                  earlier.swap(it->second.announce);
                  waiting_.erase(it);
               }
            }

            Run_(earlier);
            Run_(announce);
            return;
         }

         std::function<void()> evicted;
         bool admitted = false;

         {
            std::lock_guard<std::mutex> guard(lock_);

            admitted = Admit_(key, address, evicted);
            if (admitted)
            {
               Waiting &waiting = waiting_[key];
               if (waiting.announce)
               {
                  // A second ban of an address still waiting: both lines, when
                  // the answer does not spare it.
                  const std::function<void()> first = waiting.announce;
                  const std::function<void()> second = announce;
                  waiting.announce = [first, second]() { first(); second(); };
               }
               else
               {
                  waiting.announce = announce;
                  waiting.due = s_.now();
               }

               // After the range was saved, which every writer does before it
               // calls this: a sweep that began reading the ranges before now
               // may not have seen it (Sweep).
               waiting.filled = next_fill_++;
            }
         }

         Run_(evicted);

         if (!admitted)
         {
            if (isKnown && known.spared)
            {
               // Never announced. Lifted off this thread when there is a
               // thread to lift it on, which a spam trap's RCPT TO must not
               // wait for; here when there is none.
               Surroundings &s = s_;
               bool posted = false;
               try
               {
                  posted = s_.post([&s, address, known]() { s.lift(address, known); });
               }
               catch (...)
               {
                  posted = false;
               }

               if (!posted)
                  s_.lift(address, known);

               return;
            }

            LOG_DEBUG(_T("NeverBanNameList - too many bans wait for the never-ban list's answer; the ban of ") +
                      CanonicalText(address) + _T(" is announced at once."));
            Run_(announce);
            return;
         }

         Work_();
      }

      bool IsPending(const IPAddress &address)
      {
         std::lock_guard<std::mutex> guard(lock_);
         return waiting_.find(NeverBanNameList::Key_(address)) != waiting_.end();
      }

      // Once a minute (NeverBanNameList::RetryUnverified).
      void Sweep()
      {
         // The bans filled before the ranges are read: only their ranges are
         // sure to be in what is read. A ban written while they are read may be
         // missing from it, and is not taken for one whose range has gone.
         unsigned long long readFrom = 0;

         {
            std::lock_guard<std::mutex> guard(lock_);
            if (waiting_.empty())
               return;

            readFrom = next_fill_;
         }

         std::set<String> live;
         bool read = false;
         try
         {
            read = s_.live(live);
         }
         catch (...)
         {
            read = false;
         }

         std::vector<std::function<void()> > due;

         {
            std::lock_guard<std::mutex> guard(lock_);
            const time_t now = s_.now();

            for (auto it = waiting_.begin(); it != waiting_.end();)
            {
               Waiting &waiting = it->second;

               if (waiting.taken)
               {
                  ++it;
                  continue;
               }

               // Held, and the ban never followed: the writer stopped between
               // the two.
               if (!waiting.announce)
               {
                  if (now - waiting.held >= NeverBanNameList::RetrySeconds)
                     it = waiting_.erase(it);
                  else
                     ++it;
                  continue;
               }

               // Its range has gone - expired, or deleted by an administrator:
               // there is nothing left to verify, or to announce.
               if (read && waiting.filled < readFrom && live.find(it->first) == live.end())
               {
                  LOG_DEBUG(_T("NeverBanNameList - the ban of ") + it->first + _T(" ended before the never-ban list's answer was in; it is not announced."));
                  it = waiting_.erase(it);
                  continue;
               }

               // A minute past its time without a turn is an attempt, so that a
               // queue kept busy cannot keep a ban waiting for longer than its
               // attempts allow.
               if (now - waiting.due >= NeverBanNameList::RetrySeconds)
               {
                  waiting.attempts++;
                  waiting.due = now;
               }

               if (waiting.attempts >= NeverBanNameList::MaxAttempts || now >= waiting.deadline)
               {
                  if (!waiting.spared)
                  {
                     LOG_DEBUG(_T("NeverBanNameList - the never-ban list could not answer for ") + it->first + _T(" in ") +
                               StringParser::IntToString(waiting.attempts) + _T(" attempt(s); its ban is announced."));
                     due.push_back(waiting.announce);
                  }

                  it = waiting_.erase(it);
                  continue;
               }

               ++it;
            }
         }

         for (const std::function<void()> &announce : due)
            Run_(announce);

         Work_();
      }

      size_t Count()
      {
         std::lock_guard<std::mutex> guard(lock_);
         return waiting_.size();
      }

      int Working()
      {
         std::lock_guard<std::mutex> guard(lock_);
         return working_;
      }

   private:

      struct Waiting
      {
         // Empty while only held (Hold): VerifyBan fills it.
         std::function<void()> announce;
         IPAddress address;
         String network;
         time_t held = 0;
         time_t due = 0;
         time_t deadline = 0;
         int attempts = 0;
         unsigned long long order = 0;

         // When VerifyBan last filled it (next_fill_), which is after its range
         // was saved: Sweep judges a range gone only for a ban filled before it
         // read the ranges.
         unsigned long long filled = 0;

         // A thread is asking about it now.
         bool taken = false;

         // Its answer spared it, and its range is being, or could not be,
         // deleted: never announced.
         bool spared = false;
      };

      // A thread of the verification queue, counted from the moment it is
      // asked for: given back by Drain_ when it finds nothing more to take, or
      // here when the queue destroys the work unrun.
      struct Slot
      {
         explicit Slot(NeverBanWaitingBans &owner) : owner_(owner) { }

         ~Slot()
         {
            if (!released.load())
               owner_.GiveBack_();
         }

         std::atomic<bool> released{false};

      private:
         NeverBanWaitingBans &owner_;
      };

      static Surroundings Real_();

      // With lock_ held. Whether the address waits: it already does, or there
      // is room for it among its network's MaxPendingPerNetwork and the
      // MaxPending in all - the last made, when the set is full, by giving up
      // the oldest ban that can be given up of the network that has the most
      // of those, whose announcement is handed back in evicted. One that a
      // thread is asking about, that is only held or that is spared cannot:
      // were the network with the most waiting chosen whatever they were, a
      // set full of networks of one ban each, the first ones taken, would
      // announce the next ban at once.
      bool Admit_(const String &key, const IPAddress &address, std::function<void()> &evicted)
      {
         if (waiting_.find(key) != waiting_.end())
            return true;

         const String network = NeverBanNameList::Network_(address);

         std::map<String, size_t> perNetwork;
         std::map<String, std::pair<size_t, std::map<String, Waiting>::iterator> > evictable;
         for (auto it = waiting_.begin(); it != waiting_.end(); ++it)
         {
            const Waiting &waiting = it->second;
            perNetwork[waiting.network]++;

            if (waiting.taken || !waiting.announce || waiting.spared)
               continue;

            auto found = evictable.find(waiting.network);
            if (found == evictable.end())
               evictable[waiting.network] = std::make_pair(static_cast<size_t>(1), it);
            else
            {
               found->second.first++;
               if (waiting.order < found->second.second->second.order)
                  found->second.second = it;
            }
         }

         if (perNetwork[network] >= NeverBanNameList::MaxPendingPerNetwork)
            return false;

         if (waiting_.size() >= NeverBanNameList::MaxPending)
         {
            auto oldest = waiting_.end();
            size_t most = 0;
            for (const auto &entry : evictable)
            {
               if (entry.second.first > most)
               {
                  most = entry.second.first;
                  oldest = entry.second.second;
               }
            }

            if (oldest == waiting_.end())
               return false;

            evicted.swap(oldest->second.announce);
            waiting_.erase(oldest);
         }

         const time_t now = s_.now();

         Waiting waiting;
         waiting.address = address;
         waiting.network = network;
         waiting.held = now;
         waiting.due = now;
         waiting.deadline = now + NeverBanNameList::HoldSeconds;
         waiting.order = next_order_++;

         waiting_[key] = waiting;
         return true;
      }

      // Threads set to work, one for each ban that is due, VerifyThreads at
      // most.
      void Work_()
      {
         int start = 0;

         {
            std::lock_guard<std::mutex> guard(lock_);
            const time_t now = s_.now();

            int due = 0;
            for (const auto &entry : waiting_)
            {
               if (!entry.second.taken && entry.second.announce && entry.second.due <= now)
                  due++;
            }

            start = std::min(due, NeverBanNameList::VerifyThreads - working_);
            if (start <= 0)
               return;

            working_ += start;
         }

         for (int i = 0; i < start; i++)
         {
            std::shared_ptr<Slot> slot(new Slot(*this));
            NeverBanWaitingBans *self = this;
            bool posted = false;

            try
            {
               posted = s_.post([self, slot]()
               {
                  self->Drain_();
                  slot->released.store(true);
               });
            }
            catch (...)
            {
               posted = false;
            }

            if (!posted)
            {
               slot->released.store(true);
               GiveBack_();
            }
         }
      }

      void GiveBack_()
      {
         std::lock_guard<std::mutex> guard(lock_);
         working_--;
      }

      // One thread: the bans it can take, one after another, until there is
      // none due; then its place is given back with the lock held, so that a
      // ban added after it looked finds a thread to ask for. A thread its
      // queue asks to stop - the service stopping - takes no more: the bans
      // still waiting stay as they are, and the queue's stop is not held up by
      // a lookup each.
      void Drain_()
      {
         for (;;)
         {
            if (boost::this_thread::interruption_requested())
            {
               GiveBack_();
               return;
            }

            String key;
            IPAddress address;
            if (!TakeNext_(key, address))
               return;

            try
            {
               const unsigned started = s_.generation();

               NeverBanNameList::Verdict verdict;
               if (!s_.known(address, verdict))
               {
                  verdict = s_.resolve(address);

                  // A lookup the queue's stop cut short - WithinDeadline gives
                  // up within a slice of the request - is not one the DNS left
                  // unanswered: the ban waits as it was, no attempt spent, and
                  // the thread's place is given back as at the top of the loop.
                  // Counted, a ban at its last attempt or past its time was
                  // announced unverified - the line fail2ban matches, the
                  // firewall - as the service stopped.
                  if (!verdict.definitive && boost::this_thread::interruption_requested())
                  {
                     {
                        std::lock_guard<std::mutex> guard(lock_);
                        auto it = waiting_.find(key);
                        if (it != waiting_.end())
                           it->second.taken = false;
                     }

                     GiveBack_();
                     return;
                  }

                  s_.keep(address, verdict, started);
               }

               Decide_(key, address, verdict, started);
            }
            catch (const boost::thread_interrupted &)
            {
               // The queue is stopping, inside a lookup or the database: the
               // ban waits as it was, no attempt spent, and the stop goes on
               // (the Slot gives the thread's place back).
               std::lock_guard<std::mutex> guard(lock_);
               auto it = waiting_.find(key);
               if (it != waiting_.end())
                  it->second.taken = false;
               throw;
            }
            catch (...)
            {
               // Not decided: it waits a minute, an attempt spent.
               std::lock_guard<std::mutex> guard(lock_);
               auto it = waiting_.find(key);
               if (it != waiting_.end())
               {
                  it->second.taken = false;
                  it->second.attempts++;
                  it->second.due = s_.now() + NeverBanNameList::RetrySeconds;
               }
            }
         }
      }

      // The ban due that waited longest in the network after the one taken
      // last: each network in turn, so that one network's bans, however many,
      // delay another network's by one lookup each.
      bool TakeNext_(String &key, IPAddress &address)
      {
         std::lock_guard<std::mutex> guard(lock_);
         const time_t now = s_.now();

         std::map<String, std::map<String, Waiting>::iterator> oldest;
         for (auto it = waiting_.begin(); it != waiting_.end(); ++it)
         {
            const Waiting &waiting = it->second;
            if (waiting.taken || !waiting.announce || waiting.due > now)
               continue;

            auto found = oldest.find(waiting.network);
            if (found == oldest.end() || waiting.order < found->second->second.order)
               oldest[waiting.network] = it;
         }

         if (oldest.empty())
         {
            working_--;
            return false;
         }

         auto pick = oldest.upper_bound(last_network_);
         if (pick == oldest.end())
            pick = oldest.begin();

         last_network_ = pick->first;
         pick->second->second.taken = true;
         key = pick->second->first;
         address = pick->second->second.address;
         return true;
      }

      void Decide_(const String &key, const IPAddress &address, const NeverBanNameList::Verdict &verdict, unsigned started)
      {
         std::function<void()> announce;
         bool lift = false;

         {
            std::lock_guard<std::mutex> guard(lock_);

            auto it = waiting_.find(key);
            if (it == waiting_.end())
               return;

            Waiting &waiting = it->second;
            const bool current = started == s_.generation();

            if (verdict.spared && current)
            {
               // Kept, and left taken, while its range is deleted: the
               // reconciliation must not mirror it in between.
               lift = true;
               waiting.spared = true;
            }
            else if (verdict.definitive && current)
            {
               announce.swap(waiting.announce);
               waiting_.erase(it);
            }
            else
            {
               // Not answered, or answered under rows that have changed since:
               // asked again - at once for new rows, a minute later for a
               // lookup that was not answered - until its attempts are spent.
               waiting.taken = false;
               waiting.attempts++;

               const time_t now = s_.now();
               if (waiting.attempts >= NeverBanNameList::MaxAttempts || now >= waiting.deadline)
               {
                  LOG_DEBUG(_T("NeverBanNameList - the reverse lookup of ") + key + _T(" was not answered in ") +
                            StringParser::IntToString(waiting.attempts) + _T(" attempt(s); its ban is announced."));
                  announce.swap(waiting.announce);
                  waiting_.erase(it);
               }
               else
               {
                  waiting.due = current ? now + NeverBanNameList::RetrySeconds : now;
               }
            }
         }

         if (lift)
         {
            bool lifted = false;
            try
            {
               lifted = s_.lift(address, verdict);
            }
            catch (const boost::thread_interrupted &)
            {
               // The queue is stopping: still spared and never announced, and
               // lifted by the next start's worker.
               std::lock_guard<std::mutex> guard(lock_);
               auto it = waiting_.find(key);
               if (it != waiting_.end())
                  it->second.taken = false;
               throw;
            }
            catch (...)
            {
               lifted = false;
            }

            std::lock_guard<std::mutex> guard(lock_);
            auto it = waiting_.find(key);
            if (it == waiting_.end())
               return;

            if (lifted)
            {
               waiting_.erase(it);
               return;
            }

            // Nothing deleted: the range could not be, which Lift_ has
            // reported (HM7641), or it was gone already, which the expiry pass
            // finds. Asked again in a minute, never announced; given up after
            // its attempts or its time.
            it->second.taken = false;
            it->second.attempts++;
            it->second.due = s_.now() + NeverBanNameList::RetrySeconds;
            if (it->second.attempts >= NeverBanNameList::MaxAttempts || s_.now() >= it->second.deadline)
               waiting_.erase(it);
            return;
         }

         Run_(announce);
      }

      static void Run_(const std::function<void()> &announce)
      {
         if (!announce)
            return;

         try
         {
            announce();
         }
         catch (const boost::thread_interrupted &)
         {
            // A worker of a queue that is stopping: passed on, as DoWork does.
            throw;
         }
         catch (...)
         {
            // The ban is in the table whatever happened in its announcement,
            // and the next reconciliation mirrors it.
            LOG_DEBUG(_T("NeverBanNameList - announcing a ban ended in an error; the ban stands at the server."));
         }
      }

      Surroundings s_;
      std::mutex lock_;
      std::map<String, Waiting> waiting_;
      String last_network_;
      int working_ = 0;
      unsigned long long next_order_ = 0;
      unsigned long long next_fill_ = 1;
   };

   NeverBanWaitingBans::Surroundings
   NeverBanWaitingBans::Real_()
   {
      Surroundings real;

      real.rows = [](std::vector<String> &patterns) { return NeverBanNameList::EnsureRows_(patterns); };
      real.known = [](const IPAddress &address, NeverBanNameList::Verdict &verdict) { return NeverBanNameList::Known_(address, verdict, false); };
      real.resolve = [](const IPAddress &address) { return NeverBanNameList::ResolveWithinDeadline_(address); };
      real.keep = [](const IPAddress &address, const NeverBanNameList::Verdict &verdict, unsigned started) { NeverBanNameList::Keep_(address, verdict, started); };
      real.generation = []() { return NeverBanNameList::Generation_(); };
      real.lift = [](const IPAddress &address, const NeverBanNameList::Verdict &verdict) { return NeverBanNameList::Lift_(address, verdict); };

      real.live = [](std::set<String> &addresses) -> bool
      {
         SecurityRanges ranges;
         if (!ranges.Refresh())
            return false;

         const DateTime now = DateTime::GetCurrentTime();

         for (std::shared_ptr<SecurityRange> range : ranges.GetSnapshot())
         {
            // What an auto-ban writes, as Lift_ reads it: a single address,
            // expiring, under the name prefix AutoBanFirewall mirrors - and
            // not expired yet.
            if (!range || !range->GetExpires() || !range->GetName().StartsWith(RangeNamePrefix))
               continue;

            if (Canonical(range->GetLowerIP()) != Canonical(range->GetUpperIP()))
               continue;

            if (!(now < range->GetExpiresTime()))
               continue;

            addresses.insert(CanonicalText(range->GetLowerIP()));
         }

         return true;
      };

      real.post = [](const std::function<void()> &work) -> bool
      {
         std::shared_ptr<WorkQueue> queue = WorkQueueManager::Instance()->GetQueue(NeverBanNameList::QueueName());
         if (!queue)
            return false;

         queue->AddTask(std::shared_ptr<Task>(new NeverBanTask(work)));
         return true;
      };

      real.now = []() { return time(nullptr); };

      return real;
   }

   // ---- the rows -------------------------------------------------------------

   void
   NeverBanNameList::SetNeedRefresh()
   {
      {
         std::lock_guard<std::mutex> guard(rows_mutex);
         need_refresh = true;
      }

      Forget_();
   }

   bool
   NeverBanNameList::Refresh_()
   {
      // Called with rows_mutex held.
      NeverBanNames all;

      const time_t now = time(nullptr);

      if (!all.Refresh())
      {
         // Keep what is here: replacing the rows with none would ban a listed
         // provider for as long as the database was unwell.
         last_refresh_failure = now;

         if (last_refresh_report == 0 || now - last_refresh_report > 3600)
         {
            last_refresh_report = now;
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7640, "NeverBanNameList::Refresh_",
               "The never-ban list by reverse name could not be read. The rows last read are still in force; a row added or removed since is not in effect yet.");
         }

         return false;
      }

      std::vector<String> fresh;

      for (std::shared_ptr<NeverBanName> row : all.GetSnapshot())
      {
         if (row && !row->GetPattern().IsEmpty())
            fresh.push_back(row->GetPattern());
      }

      // A periodic re-read that finds the rows changed - another node, or a
      // statement run against the table by hand - forgets every verdict, as a
      // change made here does.
      const bool changed = fresh != row_patterns;

      row_patterns.swap(fresh);
      need_refresh = false;
      last_read = now;
      last_refresh_failure = 0;

      if (changed)
         Forget_();

      return true;
   }

   bool
   NeverBanNameList::EnsureRows_(std::vector<String> &patterns)
   {
      std::lock_guard<std::mutex> guard(rows_mutex);

      const time_t now = time(nullptr);

      const bool due = need_refresh || last_read == 0 || now - last_read >= RereadSeconds;
      const bool backingOff = last_refresh_failure != 0 && now - last_refresh_failure < 60;

      if (due && !backingOff)
         Refresh_();

      patterns = row_patterns;

      return !patterns.empty();
   }

   bool
   NeverBanNameList::HasEntries()
   {
      std::vector<String> patterns;
      return EnsureRows_(patterns);
   }

   std::vector<String>
   NeverBanNameList::Patterns()
   {
      std::vector<String> patterns;
      EnsureRows_(patterns);
      return patterns;
   }

   AnsiString
   NeverBanNameList::Document(bool &readable)
   {
      NeverBanNames all;
      readable = all.Refresh();

      AnsiString json = "{\"items\":[";
      int count = 0;

      if (readable)
      {
         for (std::shared_ptr<NeverBanName> row : all.GetSnapshot())
         {
            if (!row)
               continue;

            AnsiString item;
            item.Format("%s{\"id\":%I64d,\"pattern\":\"%s\",\"note\":\"%s\",\"created\":\"%s\"}",
               count > 0 ? "," : "",
               row->GetID(),
               JsonText(row->GetPattern()).c_str(),
               JsonText(row->GetNote()).c_str(),
               JsonText(Time::GetTimeStampFromDateTime(row->GetCreated())).c_str());

            json += item;
            count++;
         }
      }

      json += "]}";

      return json;
   }

   bool
   NeverBanNameList::Add(const String &pattern, const String &note, __int64 &id, String &refusal)
   {
      id = 0;
      refusal.Empty();

      String text = note;
      text.Trim();

      std::shared_ptr<NeverBanName> row(new NeverBanName);
      row->SetPattern(pattern);
      row->SetNote(text);

      String error;
      if (!PersistentNeverBanName::SaveObject(row, error, PersistenceModeNormal))
      {
         refusal = error.IsEmpty() ? String(_T("the entry could not be saved; see the error log")) : error;
         return false;
      }

      id = row->GetID();

      LOG_APPLICATION(_T("Never-ban list: ") + row->GetPattern() +
                      _T(" added. An address whose reverse name it covers, and which that name resolves back to, is not auto-banned."));

      return true;
   }

   bool
   NeverBanNameList::Remove(__int64 id, bool &found)
   {
      found = false;

      NeverBanNames all;
      if (!all.Refresh())
         return false;

      std::shared_ptr<NeverBanName> row = all.GetItemByDBID(id);
      if (!row)
         return true;

      found = true;

      if (!PersistentNeverBanName::DeleteObject(row))
         return false;

      LOG_APPLICATION(_T("Never-ban list: ") + row->GetPattern() + _T(" removed."));

      return true;
   }

   // ---- the verdicts ----------------------------------------------------------

   unsigned
   NeverBanNameList::Generation_()
   {
      return generation.load();
   }

   String
   NeverBanNameList::Key_(const IPAddress &address)
   {
      return CanonicalText(address);
   }

   void
   NeverBanNameList::Forget_()
   {
      std::lock_guard<std::mutex> guard(verdict_mutex);
      generation++;
      kept.clear();
   }

   size_t
   NeverBanNameList::KnownCount_()
   {
      std::lock_guard<std::mutex> guard(verdict_mutex);
      return kept.size();
   }

   void
   NeverBanNameList::Keep_(const IPAddress &address, const Verdict &verdict, unsigned startedUnder)
   {
      if (!verdict.definitive)
         return;

      std::lock_guard<std::mutex> guard(verdict_mutex);

      if (startedUnder != generation.load())
         return;

      const time_t now = time(nullptr);

      if (kept.size() >= MaxKept)
      {
         for (auto it = kept.begin(); it != kept.end();)
         {
            if (it->second.expires <= now)
               it = kept.erase(it);
            else
               ++it;
         }

         if (kept.size() >= MaxKept)
            kept.clear();
      }

      Kept entry;
      entry.verdict = verdict;
      entry.verdict.first_use = false;
      entry.expires = now + (verdict.spared ? KeepSparedSeconds : KeepRefusedSeconds);
      entry.used = false;

      kept[Key_(address)] = entry;
   }

   bool
   NeverBanNameList::Known_(const IPAddress &address, Verdict &verdict, bool markUsed)
   {
      std::lock_guard<std::mutex> guard(verdict_mutex);

      auto it = kept.find(Key_(address));
      if (it == kept.end())
         return false;

      if (it->second.expires <= time(nullptr))
      {
         kept.erase(it);
         return false;
      }

      verdict = it->second.verdict;
      verdict.first_use = !it->second.used;

      if (markUsed)
         it->second.used = true;

      return true;
   }

   // ---- the ban writers ---------------------------------------------------------

   bool
   NeverBanNameList::IsSpared(const IPAddress &address, Verdict &verdict)
   {
      Verdict known;
      if (!Known_(address, known, true))
         return false;

      if (!known.spared)
         return false;

      verdict = known;
      return true;
   }

   void
   NeverBanNameList::Hold(const IPAddress &address)
   {
      NeverBanWaitingBans::Global().Hold(address);
   }

   void
   NeverBanNameList::Release(const IPAddress &address)
   {
      NeverBanWaitingBans::Global().Release(address);
   }

   void
   NeverBanNameList::VerifyBan(const IPAddress &address, const std::function<void()> &announce)
   {
      NeverBanWaitingBans::Global().VerifyBan(address, announce);
   }

   bool
   NeverBanNameList::IsPending(const IPAddress &address)
   {
      return NeverBanWaitingBans::Global().IsPending(address);
   }

   const wchar_t *
   NeverBanNameList::QueueName()
   {
      return L"Never-ban verification queue";
   }

   void
   NeverBanNameList::Learn(const IPAddress &address, const std::vector<String> &reverseNames, bool answered)
   {
      std::vector<String> patterns;
      if (!EnsureRows_(patterns))
         return;

      Verdict known;
      if (Known_(address, known, false))
         return;

      const unsigned started = Generation_();

      Verdict verdict = Evaluate(address, reverseNames, answered, patterns, &NeverBanNameList::RealForwardLookup_);

      Keep_(address, verdict, started);

      if (verdict.spared)
      {
         LOG_DEBUG(_T("NeverBanNameList - ") + CanonicalText(address) + _T(" is spared from auto-ban: ") + verdict.reason);
      }
   }

   void
   NeverBanNameList::RetryUnverified()
   {
      NeverBanWaitingBans::Global().Sweep();
   }

   String
   NeverBanNameList::Network_(const IPAddress &address)
   {
      const boost::asio::ip::address value = Canonical(address);

      AnsiString network;
      if (value.is_v4())
      {
         const boost::asio::ip::address_v4::bytes_type bytes = value.to_v4().to_bytes();
         network.Format("%u.%u.%u.0/24", (unsigned) bytes[0], (unsigned) bytes[1], (unsigned) bytes[2]);
      }
      else
      {
         const boost::asio::ip::address_v6::bytes_type bytes = value.to_v6().to_bytes();
         network.Format("%x:%x:%x::/48", (unsigned) ((bytes[0] << 8) | bytes[1]), (unsigned) ((bytes[2] << 8) | bytes[3]),
                        (unsigned) ((bytes[4] << 8) | bytes[5]));
      }

      return String(network);
   }

   NeverBanNameList::Verdict
   NeverBanNameList::ResolveWithinDeadline_(const IPAddress &address)
   {
      std::vector<String> patterns;
      if (!EnsureRows_(patterns))
      {
         Verdict empty;
         empty.definitive = true;
         empty.reason = _T("the never-ban list is empty");
         return empty;
      }

      const int milliseconds = LookupDeadlineSeconds * 1000;

      // Each lookup on a thread of its own, held to the deadline whatever the
      // resolver does; what the lookups capture is copied there.
      const String text = CanonicalText(address);
      std::vector<String> names;
      const bool answered = WithinDeadline([text](std::vector<String> &found) -> bool
      {
         DNSResolver resolver;
         return resolver.GetPTRRecords(text, found);
      }, names, milliseconds);

      ForwardLookup forward = [milliseconds](const String &name, bool ipv6, std::vector<String> &addresses) -> bool
      {
         return WithinDeadline([name, ipv6](std::vector<String> &found) -> bool
         {
            return RealForwardLookup_(name, ipv6, found);
         }, addresses, milliseconds);
      };

      return Evaluate(address, names, answered, patterns, forward);
   }

   bool
   NeverBanNameList::RealForwardLookup_(const String &name, bool ipv6, std::vector<String> &addresses)
   {
      // The resource-record numbers of RFC 1035 and RFC 3596, as SPFDnsResolver
      // writes them: the numbers on the wire, the same on every platform.
      const int ResourceTypeA = 0x0001;
      const int ResourceTypeAAAA = 0x001c;

      DNSResolver resolver;
      std::vector<AnsiString> values;

      if (!resolver.GetRecordsOfType(AnsiString(name), ipv6 ? ResourceTypeAAAA : ResourceTypeA, values))
         return false;

      for (const AnsiString &value : values)
         addresses.push_back(String(value));

      return true;
   }

   bool
   NeverBanNameList::Lift_(const IPAddress &address, const Verdict &verdict)
   {
      const boost::asio::ip::address target = Canonical(address);
      const String text = CanonicalText(address);

      SecurityRanges ranges;
      if (!ranges.Refresh())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7641, "NeverBanNameList::Lift_",
            "The auto-ban of " + text + " could not be lifted although the never-ban list spares it (" + verdict.reason +
            "): the IP ranges could not be read. Delete its \"Auto-ban:\" range on the IP ranges page to lift it now.");
         return false;
      }

      int lifted = 0;
      bool failed = false;

      for (std::shared_ptr<SecurityRange> range : ranges.GetSnapshot())
      {
         // Only what an auto-ban writes: a single address, expiring, under the
         // name prefix AutoBanFirewall mirrors. A range an administrator wrote
         // is theirs, whatever it covers.
         if (!range || !range->GetExpires())
            continue;

         if (!range->GetName().StartsWith(RangeNamePrefix))
            continue;

         if (Canonical(range->GetLowerIP()) != target || Canonical(range->GetUpperIP()) != target)
            continue;

         // Deleting the range tells the firewall (PersistentSecurityRange::DeleteObject).
         if (PersistentSecurityRange::DeleteObject(range))
            lifted++;
         else
            failed = true;
      }

      if (failed)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7641, "NeverBanNameList::Lift_",
            "The auto-ban of " + text + " could not be lifted although the never-ban list spares it (" + verdict.reason +
            "): its range could not be deleted. Delete its \"Auto-ban:\" range on the IP ranges page to lift it now.");
      }

      // Not the shape the packaged fail2ban filter matches ("is banned for"),
      // on purpose: nothing below the server is to act on this address.
      if (lifted > 0)
      {
         LOG_APPLICATION(_T("Auto-ban: ") + text + _T(" was not banned: ") + verdict.reason + _T("; the ban written for it was lifted."));
      }

      return lifted > 0;
   }

   // ---- diagnosis -------------------------------------------------------------

   bool
   NeverBanNameList::Check(const String &addressText, AnsiString &document, String &refusal)
   {
      String text = addressText;
      text.Trim();

      IPAddress address;
      if (text.IsEmpty() || !address.TryParse(AnsiString(text), false))
      {
         refusal = _T("address must be an IPv4 or IPv6 address, such as 192.0.2.10 or 2001:db8::25");
         return false;
      }

      std::vector<String> patterns;
      EnsureRows_(patterns);

      std::vector<String> names;
      DNSResolver resolver;
      const bool answered = resolver.GetPTRRecords(CanonicalText(address), names);

      // Each name's forward lookup made once, for the verdict and for the list
      // below alike: a check is answered while a person or a REST client waits.
      std::map<String, std::pair<bool, std::vector<String> > > looked;
      ForwardLookup once = [&looked](const String &name, bool ipv6, std::vector<String> &addresses) -> bool
      {
         auto it = looked.find(name);
         if (it == looked.end())
         {
            std::vector<String> found;
            const bool ok = RealForwardLookup_(name, ipv6, found);
            it = looked.insert(std::make_pair(name, std::make_pair(ok, found))).first;
         }

         addresses = it->second.second;
         return it->second.first;
      };

      Verdict verdict;
      if (patterns.empty())
      {
         verdict.definitive = true;
         verdict.reason = _T("the never-ban list is empty");
      }
      else
      {
         verdict = Evaluate(address, names, answered, patterns, once);
      }

      // Every reverse name, with what the list makes of it - including whether it
      // resolves back to the address, looked up whether a row covers it or not,
      // since that is what tells an administrator whether a row WOULD work.
      const boost::asio::ip::address canonical = Canonical(address);
      const bool ipv6 = canonical.is_v6();

      AnsiString list = "[";
      int count = 0;
      int lookups = 0;

      for (const String &raw : names)
      {
         if (count >= MaxReverseNames)
            break;

         const String name = NormaliseName(raw);
         const bool usable = IsUsableReverseName(name);
         const String pattern = usable ? CoveringPattern(name, patterns) : String();

         AnsiString confirmed = "null";
         if (usable && (looked.count(name) > 0 || lookups < MaxForwardLookups))
         {
            if (looked.count(name) == 0)
               lookups++;
            std::vector<String> forward;
            if (once(name, ipv6, forward))
            {
               bool same = false;
               for (const String &answer : forward)
               {
                  if (SameAddress(address, answer))
                     same = true;
               }
               confirmed = same ? "true" : "false";
            }
         }

         AnsiString item;
         item.Format("%s{\"name\":%s,\"usable\":%s,\"pattern\":%s,\"resolves_back\":%s}",
            count > 0 ? "," : "",
            JsonQuoted(name).c_str(),
            JsonFlag(usable),
            pattern.IsEmpty() ? "null" : JsonQuoted(pattern).c_str(),
            confirmed.c_str());

         list += item;
         count++;
      }

      list += "]";

      document.Format("{\"address\":%s,\"reverse_names\":%s,\"answered\":%s,\"spared\":%s,\"name\":%s,\"pattern\":%s,\"reason\":%s}",
         JsonQuoted(CanonicalText(address)).c_str(),
         list.c_str(),
         JsonFlag(answered),
         JsonFlag(verdict.spared),
         verdict.name.IsEmpty() ? "null" : JsonQuoted(verdict.name).c_str(),
         verdict.pattern.IsEmpty() ? "null" : JsonQuoted(verdict.pattern).c_str(),
         JsonQuoted(verdict.reason).c_str());

      return true;
   }

   // ---- the rules ---------------------------------------------------------------

   String
   NeverBanNameList::NormaliseName(const String &name)
   {
      String value = name;
      value.Trim();
      value.ToLower();

      while (value.GetLength() > 0 && value.EndsWith(_T(".")))
         value = value.Left(value.GetLength() - 1);

      return value;
   }

   bool
   NeverBanNameList::IsUsableReverseName(const String &normalisedName)
   {
      if (normalisedName.IsEmpty() || normalisedName.GetLength() > 255)
         return false;

      // A host name is printable ASCII. Anything else - a control character, a
      // quote, a byte a reverse zone's holder put there - is matched by nothing
      // and never reaches a log line as it came.
      for (int i = 0; i < normalisedName.GetLength(); i++)
      {
         const wchar_t c = normalisedName.GetAt(i);
         if (c < 0x21 || c > 0x7E || c == L'"' || c == L'\\')
            return false;
      }

      // A reverse zone that answers with an address written as a name - some do
      // - names nobody.
      if (StringParser::IsValidIPAddress(normalisedName))
         return false;

      if (normalisedName.StartsWith(_T(".")) || normalisedName.Find(_T("..")) >= 0)
         return false;

      if (normalisedName.Find(_T(" ")) >= 0 || normalisedName.Find(_T("@")) >= 0 ||
          normalisedName.Find(_T("[")) >= 0 || normalisedName.Find(_T("*")) >= 0)
         return false;

      // localhost, a top-level domain, co.uk: a name nobody registers, so no
      // pattern may spare it and none can.
      if (NeverBanName::IsPublicSuffix(normalisedName))
         return false;

      return true;
   }

   String
   NeverBanNameList::CoveringPattern(const String &normalisedName, const std::vector<String> &patterns)
   {
      String best;
      bool bestIsSuffix = false;

      for (const String &pattern : patterns)
      {
         if (!NeverBanName::PatternCovers(pattern, normalisedName))
            continue;

         const bool suffix = pattern.StartsWith(_T("."));

         if (best.IsEmpty() ||
             (bestIsSuffix && !suffix) ||
             (bestIsSuffix == suffix && pattern.GetLength() > best.GetLength()))
         {
            best = pattern;
            bestIsSuffix = suffix;
         }
      }

      return best;
   }

   bool
   NeverBanNameList::SameAddress(const IPAddress &address, const String &answer)
   {
      String text = answer;
      text.Trim();

      IPAddress parsed;
      if (text.IsEmpty() || !parsed.TryParse(AnsiString(text), false))
         return false;

      return Canonical(address) == Canonical(parsed);
   }

   NeverBanNameList::Verdict
   NeverBanNameList::Evaluate(const IPAddress &address, const std::vector<String> &reverseNames, bool answered,
                              const std::vector<String> &patterns, const ForwardLookup &forward)
   {
      Verdict verdict;

      if (patterns.empty())
      {
         verdict.definitive = true;
         verdict.reason = _T("the never-ban list is empty");
         return verdict;
      }

      if (!answered)
      {
         verdict.definitive = false;
         verdict.reason = _T("its reverse lookup was not answered");
         return verdict;
      }

      if (reverseNames.empty())
      {
         verdict.definitive = true;
         verdict.reason = _T("it has no reverse name");
         return verdict;
      }

      const bool ipv6 = Canonical(address).is_v6();

      bool allAnswered = true;
      int lookups = 0;
      int looked = 0;

      String firstUsable;
      String unusable;
      String nearName;
      String nearPattern;
      bool nearUnanswered = false;

      for (const String &raw : reverseNames)
      {
         if (looked >= MaxReverseNames)
            break;

         looked++;

         const String name = NormaliseName(raw);

         if (!IsUsableReverseName(name))
         {
            if (unusable.IsEmpty())
               unusable = name;
            continue;
         }

         if (firstUsable.IsEmpty())
            firstUsable = name;

         const String pattern = CoveringPattern(name, patterns);
         if (pattern.IsEmpty())
            continue;

         if (lookups >= MaxForwardLookups)
         {
            allAnswered = false;
            continue;
         }

         lookups++;

         std::vector<String> addresses;
         if (!forward || !forward(name, ipv6, addresses))
         {
            allAnswered = false;

            if (nearName.IsEmpty())
            {
               nearName = name;
               nearPattern = pattern;
               nearUnanswered = true;
            }

            continue;
         }

         for (const String &answer : addresses)
         {
            if (!SameAddress(address, answer))
               continue;

            verdict.spared = true;
            verdict.definitive = true;
            verdict.name = name;
            verdict.pattern = pattern;
            verdict.reason = _T("its reverse name ") + name + _T(" resolves back to it and is covered by ") + pattern +
                             _T(" on the never-ban list");
            return verdict;
         }

         if (nearName.IsEmpty() || nearUnanswered)
         {
            nearName = name;
            nearPattern = pattern;
            nearUnanswered = false;
         }
      }

      verdict.definitive = allAnswered;
      verdict.name = nearName.IsEmpty() ? firstUsable : nearName;
      verdict.pattern = nearPattern;

      if (!nearName.IsEmpty() && !nearUnanswered)
      {
         verdict.reason = _T("its reverse name ") + nearName + _T(" is covered by ") + nearPattern +
                          _T(" on the never-ban list but does not resolve back to it, so anybody could have written it");
      }
      else if (!nearName.IsEmpty())
      {
         verdict.reason = _T("its reverse name ") + nearName + _T(" is covered by ") + nearPattern +
                          _T(" on the never-ban list but its forward lookup was not answered");
      }
      else if (!firstUsable.IsEmpty())
      {
         verdict.reason = _T("its reverse name ") + firstUsable + _T(" is not covered by the never-ban list");
      }
      else
      {
         verdict.reason = _T("its reverse name ") + Printable(unusable) + _T(" is an address, a public suffix or not a host name, which the never-ban list never matches");
      }

      return verdict;
   }

   // ---- the tester ----------------------------------------------------------------

   namespace
   {
      void Expect(bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(what);
      }

      IPAddress Address(const char *text)
      {
         IPAddress address;
         if (!address.TryParse(AnsiString(text), false))
            throw std::logic_error("NeverBanNameListTester: a test address did not parse.");
         return address;
      }

      // A forward zone of the tester's own: name -> addresses, and the names
      // whose lookups fail.
      struct FakeZone
      {
         std::map<String, std::vector<String> > a;
         std::map<String, std::vector<String> > aaaa;
         std::vector<String> failing;
         int asked = 0;

         NeverBanNameList::ForwardLookup Lookup()
         {
            return [this](const String &name, bool ipv6, std::vector<String> &out) -> bool
            {
               asked++;

               for (const String &bad : failing)
               {
                  if (bad == name)
                     return false;
               }

               const std::map<String, std::vector<String> > &zone = ipv6 ? aaaa : a;
               auto it = zone.find(name);
               if (it != zone.end())
                  out = it->second;
               return true;
            };
         }
      };
   }

   void
   NeverBanNameListTester::Test()
   {
      // ---- patterns: what may be listed
      String refusal;

      Expect(NeverBanName::ValidatePattern(_T("mail.example.net"), refusal), "A whole host name was refused as a pattern.");
      Expect(NeverBanName::ValidatePattern(_T(".example.net"), refusal), "A suffix was refused as a pattern.");
      Expect(NeverBanName::ValidatePattern(_T(" .Example.NET. "), refusal), "A suffix in another case, with spaces and the root's dot, was refused.");
      Expect(NeverBanName::ValidatePattern(_T(".outbound.protection.outlook.com"), refusal), "A deep suffix was refused.");

      Expect(!NeverBanName::ValidatePattern(_T(""), refusal), "An empty pattern was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("."), refusal), "A lone dot was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T(".com"), refusal), "The public suffix .com was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("co.uk"), refusal), "The public suffix co.uk was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T(".github.io"), refusal), "The private-section suffix .github.io was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("localhost"), refusal), "A single label was accepted.");
      Expect(NeverBanName::ValidatePattern(_T("example.co.uk"), refusal), "A registrable domain under co.uk was refused - the negative control of the suffix rule.");
      Expect(!NeverBanName::ValidatePattern(_T("*.example.net"), refusal), "A star wildcard was accepted.");
      Expect(refusal.Find(_T(".example.net")) >= 0, "The star's refusal does not say how to write the wildcard.");

      // The refusal names the pattern that was meant, not the example alone - and
      // only one this check takes: a star anywhere else, or before a public
      // suffix, gets the example. (Before 6.3.6 every star got the example, which
      // AutoBanNeverBanNamesCom's *.nbn-com.test found on Windows.)
      Expect(!NeverBanName::ValidatePattern(_T(" *.Mail.Example.ORG. "), refusal) &&
             refusal.Find(_T("write .mail.example.org for mail.example.org ")) >= 0,
             "The star's refusal does not name the pattern that was meant.");
      Expect(!NeverBanName::ValidatePattern(_T("*.com"), refusal) && refusal.Find(_T(".com")) < 0 &&
             refusal.Find(_T(".example.net")) >= 0,
             "The star's refusal offered a public suffix as the pattern to write.");
      Expect(!NeverBanName::ValidatePattern(_T("*.*.example.org"), refusal) && refusal.Find(_T("example.org")) < 0,
             "The refusal of two stars offered a pattern with a star in it.");
      Expect(!NeverBanName::ValidatePattern(_T("mail.*.example.org"), refusal) && refusal.Find(_T("example.org")) < 0,
             "The refusal of a star inside the name offered a pattern.");
      Expect(!NeverBanName::ValidatePattern(_T("*.mail example.org"), refusal) && refusal.Find(_T("mail example")) < 0,
             "The star's refusal echoed a pattern that is no name.");
      Expect(!NeverBanName::ValidatePattern(_T("192.0.2.1"), refusal), "An IPv4 address was accepted as a name.");
      Expect(!NeverBanName::ValidatePattern(_T("2001:db8::1"), refusal), "An IPv6 address was accepted as a name.");
      Expect(!NeverBanName::ValidatePattern(_T("[192.0.2.1]"), refusal), "An address literal was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("192.0.2.0/24"), refusal), "A network was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("postmaster@example.net"), refusal), "An email address was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("mail example.net"), refusal), "A pattern with a space was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("..example.net"), refusal), "A pattern with an empty label was accepted.");
      Expect(!NeverBanName::ValidatePattern(_T("-bad.example.net"), refusal), "A label starting with a hyphen was accepted.");

      Expect(NeverBanName::NormalisePattern(_T(" .Example.NET.. ")) == _T(".example.net"), "A pattern was not normalised.");

      // ---- covering: the domain list's rules, at a label boundary
      Expect(NeverBanName::PatternCovers(_T(".example.net"), _T("mail5.outbound.example.net")), "A suffix did not cover a name beneath it.");
      Expect(NeverBanName::PatternCovers(_T(".example.net"), _T("example.net")), "A suffix did not cover its own apex.");
      Expect(!NeverBanName::PatternCovers(_T(".example.net"), _T("notexample.net")), "A suffix covered a name across a label boundary.");
      Expect(!NeverBanName::PatternCovers(_T(".example.net"), _T("example.net.attacker.test")), "A suffix covered a name that merely contains it.");
      Expect(NeverBanName::PatternCovers(_T("mail.example.net"), _T("mail.example.net")), "A whole name did not cover itself.");
      Expect(!NeverBanName::PatternCovers(_T("mail.example.net"), _T("x.mail.example.net")), "A whole name covered a name beneath it.");
      Expect(!NeverBanName::PatternCovers(_T("example.net"), _T("mail.example.net")), "A whole domain covered a host beneath it - that is what the leading dot is for.");

      // ---- reverse names that may be matched
      Expect(NeverBanNameList::NormaliseName(_T(" Mail.Example.NET. ")) == _T("mail.example.net"), "A reverse name was not normalised.");
      Expect(NeverBanNameList::IsUsableReverseName(_T("mail.example.net")), "An ordinary reverse name was not usable.");
      Expect(!NeverBanNameList::IsUsableReverseName(_T("")), "An empty reverse name was usable.");
      Expect(!NeverBanNameList::IsUsableReverseName(_T("localhost")), "localhost was usable.");
      Expect(!NeverBanNameList::IsUsableReverseName(_T("co.uk")), "A public suffix was usable.");
      Expect(!NeverBanNameList::IsUsableReverseName(_T("192.0.2.9")), "An address written as a reverse name was usable.");
      Expect(!NeverBanNameList::IsUsableReverseName(_T("mail..example.net")), "A reverse name with an empty label was usable.");
      Expect(!NeverBanNameList::IsUsableReverseName(_T("mail\r\nforged.example.net")), "A reverse name with a line break was usable.");
      Expect(!NeverBanNameList::IsUsableReverseName(_T("mail\"x.example.net")), "A reverse name with a quote was usable.");
      Expect(NeverBanNameList::IsUsableReverseName(_T("mail_relay.example.net")), "A reverse name with an underscore, which real reverse zones carry, was not usable.");

      // ---- the most specific pattern
      std::vector<String> patterns;
      patterns.push_back(_T(".example.net"));
      patterns.push_back(_T(".outbound.example.net"));
      patterns.push_back(_T("mail5.outbound.example.net"));

      Expect(NeverBanNameList::CoveringPattern(_T("mail5.outbound.example.net"), patterns) == _T("mail5.outbound.example.net"), "A whole name did not beat the suffixes covering it.");
      Expect(NeverBanNameList::CoveringPattern(_T("mail6.outbound.example.net"), patterns) == _T(".outbound.example.net"), "The longer suffix did not win.");
      Expect(NeverBanNameList::CoveringPattern(_T("www.example.net"), patterns) == _T(".example.net"), "The only covering suffix was not chosen.");
      Expect(NeverBanNameList::CoveringPattern(_T("mail.example.org"), patterns).IsEmpty(), "A pattern was found for a name nothing covers.");

      // ---- the same address
      const IPAddress v4 = Address("203.0.113.14");
      const IPAddress v6 = Address("2001:db8::25");
      const IPAddress mapped = Address("::ffff:203.0.113.14");

      Expect(NeverBanNameList::SameAddress(v4, _T("203.0.113.14")), "An address was not its own forward answer.");
      Expect(!NeverBanNameList::SameAddress(v4, _T("203.0.113.15")), "A neighbour was taken for the address.");
      Expect(NeverBanNameList::SameAddress(v6, _T("2001:DB8:0:0::25")), "An IPv6 address in another spelling was not the same.");
      Expect(!NeverBanNameList::SameAddress(v6, _T("2001:db8::26")), "Another IPv6 address was taken for the address.");
      Expect(NeverBanNameList::SameAddress(mapped, _T("203.0.113.14")), "A mapped IPv4 address was not its IPv4 answer.");
      Expect(!NeverBanNameList::SameAddress(v4, _T("not an address")), "Text that is not an address matched.");

      // ---- the decision
      FakeZone zone;
      zone.a[_T("mail5.outbound.example.net")].push_back(_T("203.0.113.14"));
      zone.a[_T("spoofed.example.net")].push_back(_T("198.51.100.1"));
      zone.aaaa[_T("mx.example.net")].push_back(_T("2001:db8::25"));
      zone.failing.push_back(_T("broken.example.net"));

      std::vector<String> list;
      list.push_back(_T(".example.net"));

      std::vector<String> names;
      names.push_back(_T("MAIL5.outbound.example.net."));

      NeverBanNameList::Verdict verdict = NeverBanNameList::Evaluate(v4, names, true, list, zone.Lookup());
      Expect(verdict.spared && verdict.definitive, "A covered, forward-confirmed reverse name did not spare its address.");
      Expect(verdict.name == _T("mail5.outbound.example.net") && verdict.pattern == _T(".example.net"), "The verdict did not name the reverse name and the pattern.");

      // Negative controls of the same case, one change each.
      std::vector<String> otherList;
      otherList.push_back(_T(".example.org"));
      verdict = NeverBanNameList::Evaluate(v4, names, true, otherList, zone.Lookup());
      Expect(!verdict.spared && verdict.definitive, "An address was spared by a list that does not cover its reverse name.");

      verdict = NeverBanNameList::Evaluate(Address("203.0.113.99"), names, true, list, zone.Lookup());
      Expect(!verdict.spared && verdict.definitive, "A covered reverse name that resolves to another address spared this one.");
      Expect(verdict.reason.Find(_T("does not resolve back")) >= 0, "The refusal does not say the name did not resolve back.");

      std::vector<String> spoofed;
      spoofed.push_back(_T("spoofed.example.net"));
      verdict = NeverBanNameList::Evaluate(v4, spoofed, true, list, zone.Lookup());
      Expect(!verdict.spared, "A reverse name its forward zone does not confirm spared the address.");

      std::vector<String> broken;
      broken.push_back(_T("broken.example.net"));
      verdict = NeverBanNameList::Evaluate(v4, broken, true, list, zone.Lookup());
      Expect(!verdict.spared && !verdict.definitive, "A forward lookup that failed was taken as an answer.");

      verdict = NeverBanNameList::Evaluate(v4, names, false, list, zone.Lookup());
      Expect(!verdict.spared && !verdict.definitive, "A reverse lookup that failed was taken as an answer.");

      verdict = NeverBanNameList::Evaluate(v4, std::vector<String>(), true, list, zone.Lookup());
      Expect(!verdict.spared && verdict.definitive, "An address with no reverse name was spared, or its absence not taken as an answer.");

      verdict = NeverBanNameList::Evaluate(v4, names, true, std::vector<String>(), zone.Lookup());
      Expect(!verdict.spared, "An empty list spared an address.");

      // An address written as its own reverse name, and a bare suffix, are never
      // looked up at all.
      std::vector<String> literal;
      literal.push_back(_T("203.0.113.14"));
      literal.push_back(_T("net"));
      zone.asked = 0;
      verdict = NeverBanNameList::Evaluate(v4, literal, true, list, zone.Lookup());
      Expect(!verdict.spared && zone.asked == 0, "An address or a public suffix given as a reverse name was looked up or matched.");

      // A reverse name its zone's holder filled with a line break reaches the
      // reason - which goes into a log line - only as printable text.
      std::vector<String> hostile;
      hostile.push_back(_T("x\r\n\"Auto-ban: 192.0.2.1 is banned for 60 minutes"));
      verdict = NeverBanNameList::Evaluate(v4, hostile, true, list, zone.Lookup());
      Expect(!verdict.spared && verdict.reason.Find(_T("\r")) < 0 && verdict.reason.Find(_T("\n")) < 0 && verdict.reason.Find(_T("\"")) < 0,
             "A reverse name's line break or quote reached the verdict's reason as it came.");

      // Several reverse names: the one that qualifies spares, wherever it is.
      std::vector<String> several;
      several.push_back(_T("customer-14.isp.example"));
      several.push_back(_T("spoofed.example.net"));
      several.push_back(_T("mail5.outbound.example.net"));
      verdict = NeverBanNameList::Evaluate(v4, several, true, list, zone.Lookup());
      Expect(verdict.spared && verdict.name == _T("mail5.outbound.example.net"), "The qualifying reverse name among several did not spare the address.");

      // IPv6: AAAA, not A.
      std::vector<String> v6names;
      v6names.push_back(_T("mx.example.net"));
      verdict = NeverBanNameList::Evaluate(v6, v6names, true, list, zone.Lookup());
      Expect(verdict.spared, "An IPv6 address confirmed by its AAAA record was not spared.");
      zone.aaaa.clear();
      zone.a[_T("mx.example.net")].push_back(_T("2001:db8::25"));
      verdict = NeverBanNameList::Evaluate(v6, v6names, true, list, zone.Lookup());
      Expect(!verdict.spared, "An IPv6 address was confirmed by an A lookup.");

      // A mapped IPv4 peer is confirmed by A.
      verdict = NeverBanNameList::Evaluate(mapped, names, true, list, zone.Lookup());
      Expect(verdict.spared, "A mapped IPv4 peer was not confirmed by its A record.");

      // The forward lookups one verdict may cost are bounded.
      std::vector<String> many;
      for (int i = 0; i < 20; i++)
         many.push_back(_T("host") + StringParser::IntToString(i) + _T(".example.net"));
      zone.asked = 0;
      verdict = NeverBanNameList::Evaluate(v4, many, true, list, zone.Lookup());
      Expect(zone.asked == NeverBanNameList::MaxForwardLookups, "The forward lookups of one verdict were not bounded.");
      Expect(!verdict.spared && !verdict.definitive, "A verdict that stopped looking was taken as definitive.");

      // ---- the verdict cache, with no database and no DNS
      NeverBanNameList::Forget_();
      Expect(NeverBanNameList::KnownCount_() == 0, "Forget_ left a verdict.");

      NeverBanNameList::Verdict spared;
      spared.spared = true;
      spared.definitive = true;
      spared.name = _T("mail5.outbound.example.net");
      spared.pattern = _T(".example.net");

      const IPAddress cached = Address("192.0.2.200");
      NeverBanNameList::Keep_(cached, spared, NeverBanNameList::Generation_());

      NeverBanNameList::Verdict read;
      Expect(NeverBanNameList::IsSpared(cached, read) && read.first_use, "A kept spared verdict was not read, or not as its first use.");
      Expect(NeverBanNameList::IsSpared(cached, read) && !read.first_use, "A kept verdict's second read was called its first.");
      Expect(!NeverBanNameList::IsSpared(Address("192.0.2.201"), read), "An address nothing is known about was spared.");

      NeverBanNameList::Verdict undecided = spared;
      undecided.definitive = false;
      const IPAddress notKept = Address("192.0.2.202");
      NeverBanNameList::Keep_(notKept, undecided, NeverBanNameList::Generation_());
      Expect(!NeverBanNameList::IsSpared(notKept, read), "A verdict that was not definitive was kept.");

      const unsigned before = NeverBanNameList::Generation_();
      NeverBanNameList::Forget_();
      Expect(!NeverBanNameList::IsSpared(cached, read), "A verdict survived the rows changing.");

      NeverBanNameList::Keep_(cached, spared, before);
      Expect(!NeverBanNameList::IsSpared(cached, read), "A verdict reached under rows that have since changed was kept.");

      NeverBanNameList::Verdict refused;
      refused.definitive = true;
      NeverBanNameList::Keep_(cached, refused, NeverBanNameList::Generation_());
      Expect(!NeverBanNameList::IsSpared(cached, read), "A kept verdict that does not spare was read as sparing.");

      NeverBanNameList::Forget_();

      // ---- the bans waiting for the list's answer (NeverBanWaitingBans), with a
      // stand-in for the world around them: a resolver that hangs for an
      // address until the test lets it answer, the ranges in force, a lift that
      // records what it deleted, a clock the test moves, and threads of the
      // test's own in place of the verification queue's. Before 6.3.6 was
      // released a ban was announced at once past sixteen verifications in
      // flight, and the expiry pass mirrored a pending range in any case: an
      // outsider who kept the name lookup queue backed up had a listed
      // provider's ban reach the firewall and fail2ban unverified.
      {
         struct Bench
         {
            std::mutex lock;
            std::condition_variable changed;
            bool rows = true;
            std::map<String, NeverBanNameList::Verdict> answers;
            std::set<String> hanging;
            std::set<String> resolving;
            std::map<String, int> resolved;
            std::set<String> live;
            std::vector<String> lifted;
            std::map<String, int> announced;
            time_t clock = 1000000;
            std::vector<std::thread> threads;

            // Work held back rather than given a thread, while holdPosts is
            // set: the test runs it on a thread of its choosing.
            bool holdPosts = false;
            std::vector<std::function<void()> > heldPosts;

            // Run once, after the ranges are read and before the sweep acts
            // on what it read.
            std::function<void()> duringLive;

            // Addresses whose next lookup is interrupted, as a stopping
            // queue's worker is inside one.
            std::set<String> interrupting;
         };

         Bench bench;

         NeverBanWaitingBans::Surroundings surroundings;

         surroundings.rows = [&bench](std::vector<String> &patterns) -> bool
         {
            std::lock_guard<std::mutex> guard(bench.lock);
            patterns.clear();
            if (bench.rows)
               patterns.push_back(_T(".example.net"));
            return !patterns.empty();
         };

         surroundings.known = [](const IPAddress &, NeverBanNameList::Verdict &) { return false; };

         surroundings.resolve = [&bench](const IPAddress &address) -> NeverBanNameList::Verdict
         {
            const String key = CanonicalText(address);

            std::unique_lock<std::mutex> guard(bench.lock);
            if (bench.interrupting.erase(key) > 0)
               throw boost::thread_interrupted();

            bench.resolving.insert(key);
            bench.resolved[key]++;
            bench.changed.notify_all();

            // A lookup that hangs until the test lets it go - or two minutes,
            // so that a bench that fails cannot leave a thread behind for good.
            bench.changed.wait_for(guard, std::chrono::seconds(120), [&bench, &key]() { return bench.hanging.find(key) == bench.hanging.end(); });

            bench.resolving.erase(key);
            bench.changed.notify_all();

            auto it = bench.answers.find(key);
            if (it != bench.answers.end())
               return it->second;

            NeverBanNameList::Verdict unanswered;
            unanswered.reason = _T("its reverse lookup was not answered");
            return unanswered;
         };

         surroundings.keep = [](const IPAddress &, const NeverBanNameList::Verdict &, unsigned) { };
         surroundings.generation = []() { return 0u; };

         surroundings.lift = [&bench](const IPAddress &address, const NeverBanNameList::Verdict &) -> bool
         {
            std::lock_guard<std::mutex> guard(bench.lock);
            const String key = CanonicalText(address);
            bench.lifted.push_back(key);
            bench.live.erase(key);
            return true;
         };

         surroundings.live = [&bench](std::set<String> &addresses) -> bool
         {
            std::function<void()> during;

            {
               std::lock_guard<std::mutex> guard(bench.lock);
               addresses = bench.live;
               during.swap(bench.duringLive);
            }

            if (during)
               during();

            return true;
         };

         surroundings.post = [&bench](const std::function<void()> &work) -> bool
         {
            std::lock_guard<std::mutex> guard(bench.lock);
            if (bench.holdPosts)
               bench.heldPosts.push_back(work);
            else
               bench.threads.push_back(std::thread(work));
            return true;
         };

         surroundings.now = [&bench]() -> time_t
         {
            std::lock_guard<std::mutex> guard(bench.lock);
            return bench.clock;
         };

         NeverBanWaitingBans bans(surroundings);

         // Never called with bench.lock held, and never holding it while bans is
         // asked: the waiting bans take their lock and then the bench's.
         auto eventually = [](const std::function<bool()> &condition) -> bool
         {
            for (int i = 0; i < 6000; i++)
            {
               if (condition())
                  return true;
               std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return false;
         };

         auto read = [&bench](const std::function<bool()> &question) -> bool
         {
            std::lock_guard<std::mutex> guard(bench.lock);
            return question();
         };

         auto announcer = [&bench](const String &key) -> std::function<void()>
         {
            return [&bench, key]()
            {
               std::lock_guard<std::mutex> guard(bench.lock);
               bench.announced[key]++;
            };
         };

         auto ban = [&bench](const IPAddress &address, bool hangs, int verdict)
         {
            // verdict: 0 not answered, 1 spared, 2 not spared.
            std::lock_guard<std::mutex> guard(bench.lock);
            const String key = CanonicalText(address);
            bench.live.insert(key);
            if (hangs)
               bench.hanging.insert(key);

            if (verdict != 0)
            {
               NeverBanNameList::Verdict answer;
               answer.definitive = true;
               answer.spared = verdict == 1;
               answer.reason = verdict == 1 ? _T("spared") : _T("not covered");
               bench.answers[key] = answer;
            }
         };

         auto letGo = [&bench](const String &key)
         {
            std::lock_guard<std::mutex> guard(bench.lock);
            bench.hanging.erase(key);
            bench.changed.notify_all();
         };

         auto announcedCount = [&bench](const String &key) -> int
         {
            std::lock_guard<std::mutex> guard(bench.lock);
            auto it = bench.announced.find(key);
            return it == bench.announced.end() ? 0 : it->second;
         };

         auto wasLifted = [&bench](const String &key) -> bool
         {
            std::lock_guard<std::mutex> guard(bench.lock);
            return std::find(bench.lifted.begin(), bench.lifted.end(), key) != bench.lifted.end();
         };

         auto resolving = [&bench](const String &key) -> bool
         {
            std::lock_guard<std::mutex> guard(bench.lock);
            return bench.resolving.find(key) != bench.resolving.end();
         };

         auto resolvedTimes = [&bench](const String &key) -> int
         {
            std::lock_guard<std::mutex> guard(bench.lock);
            auto it = bench.resolved.find(key);
            return it == bench.resolved.end() ? 0 : it->second;
         };

         auto advance = [&bench](int seconds)
         {
            std::lock_guard<std::mutex> guard(bench.lock);
            bench.clock += seconds;
         };

         std::string failure;

         try
         {
            // A ban about to be written is pending before its range exists,
            // and a hold the write did not follow is let go.
            const IPAddress provider = Address("192.0.2.80");
            const String providerKey = CanonicalText(provider);
            ban(provider, true, 1);

            Expect(!bans.IsPending(provider), "An address was pending before anything was held for it - the negative control.");
            bans.Hold(provider);
            Expect(bans.IsPending(provider), "A ban about to be written was not pending before its range was written.");
            bans.Release(provider);
            Expect(!bans.IsPending(provider), "A hold whose range could not be written stayed pending.");

            // A listed provider's ban while its lookup hangs: pending - so the
            // firewall's reconciliation leaves its range out - and not
            // announced; lifted, never announced, when the answer spares it.
            bans.Hold(provider);
            bans.VerifyBan(provider, announcer(providerKey));
            Expect(eventually([&]() { return resolving(providerKey); }), "The provider's lookup was never begun.");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            Expect(bans.IsPending(provider), "A ban whose answer is not in was not pending: the reconciliation would mirror its range.");
            Expect(announcedCount(providerKey) == 0, "A ban whose answer is not in was announced to the firewall and fail2ban.");

            letGo(providerKey);
            Expect(eventually([&]() { return !bans.IsPending(provider); }), "A ban whose answer came in stayed pending.");
            Expect(wasLifted(providerKey), "A listed provider whose reverse name verified was not lifted.");
            Expect(announcedCount(providerKey) == 0, "A listed provider whose reverse name verified was announced.");

            // The negative control: an answer that does not spare announces the
            // ban, once, and lifts nothing.
            const IPAddress other = Address("192.0.2.81");
            const String otherKey = CanonicalText(other);
            ban(other, false, 2);
            bans.Hold(other);
            bans.VerifyBan(other, announcer(otherKey));
            Expect(eventually([&]() { return announcedCount(otherKey) == 1 && !bans.IsPending(other); }), "A ban the list does not spare was not announced.");
            Expect(!wasLifted(otherKey), "A ban the list does not spare was lifted.");

            // An empty list announces at once, and nothing waits.
            {
               std::lock_guard<std::mutex> guard(bench.lock);
               bench.rows = false;
            }
            const IPAddress unlisted = Address("192.0.2.84");
            const String unlistedKey = CanonicalText(unlisted);
            ban(unlisted, true, 0);
            bans.Hold(unlisted);
            Expect(!bans.IsPending(unlisted), "With no rows a ban was held.");
            bans.VerifyBan(unlisted, announcer(unlistedKey));
            Expect(announcedCount(unlistedKey) == 1 && !bans.IsPending(unlisted) && resolvedTimes(unlistedKey) == 0,
                   "With no rows a ban was not announced at once, or was looked up.");
            letGo(unlistedKey);
            {
               std::lock_guard<std::mutex> guard(bench.lock);
               bench.rows = true;
            }

            // A lookup that is not answered: asked again a minute later, and
            // the ban announced after MaxAttempts - pending, and not announced,
            // until then.
            const IPAddress mute = Address("192.0.2.82");
            const String muteKey = CanonicalText(mute);
            ban(mute, false, 0);
            bans.VerifyBan(mute, announcer(muteKey));
            for (int attempt = 1; attempt < NeverBanNameList::MaxAttempts; attempt++)
            {
               Expect(eventually([&]() { return resolvedTimes(muteKey) == attempt && bans.Working() == 0; }), "An unanswered ban was not asked about again.");
               Expect(bans.IsPending(mute) && announcedCount(muteKey) == 0, "An unanswered ban was announced before its attempts were spent.");
               advance(NeverBanNameList::RetrySeconds);
               bans.Sweep();
            }
            Expect(eventually([&]() { return announcedCount(muteKey) == 1 && !bans.IsPending(mute); }),
                   "A ban whose lookups were not answered MaxAttempts times was not announced.");
            Expect(resolvedTimes(muteKey) == NeverBanNameList::MaxAttempts, "An unanswered ban was asked about more or fewer times than MaxAttempts.");

            // A waiting ban whose range has gone - expired, or deleted - is
            // forgotten and not announced; while its range is in force, it
            // waits.
            const IPAddress gone = Address("192.0.2.83");
            const String goneKey = CanonicalText(gone);
            ban(gone, false, 0);
            bans.VerifyBan(gone, announcer(goneKey));
            Expect(eventually([&]() { return resolvedTimes(goneKey) == 1 && bans.Working() == 0; }), "The lookup of a ban was never made.");
            bans.Sweep();
            Expect(bans.IsPending(gone), "A waiting ban whose range is in force was forgotten - the negative control.");
            {
               std::lock_guard<std::mutex> guard(bench.lock);
               bench.live.erase(goneKey);
            }
            bans.Sweep();
            Expect(!bans.IsPending(gone) && announcedCount(goneKey) == 0, "A waiting ban whose range had gone was announced, or kept.");

            // A worker its queue asks to stop - the service stopping - takes no
            // more bans once the one it asks about is answered: that one is
            // decided by its answer, which here does not spare it, and the next
            // stays waiting for a worker of the next start, unasked and
            // unannounced. A worker not asked to stop takes it (the negative
            // control). The answer is one the DNS gave: a lookup the stop cuts
            // short is the case below.
            {
               const IPAddress first = Address("192.0.2.86");
               const String firstKey = CanonicalText(first);
               const IPAddress second = Address("192.0.2.87");
               const String secondKey = CanonicalText(second);

               {
                  std::lock_guard<std::mutex> guard(bench.lock);
                  bench.holdPosts = true;
               }

               ban(first, true, 2);
               ban(second, false, 2);
               bans.VerifyBan(first, announcer(firstKey));
               bans.VerifyBan(second, announcer(secondKey));

               std::vector<std::function<void()> > held;
               {
                  std::lock_guard<std::mutex> guard(bench.lock);
                  bench.holdPosts = false;
                  held.swap(bench.heldPosts);
               }
               Expect(!held.empty(), "No worker was asked for two waiting bans.");

               boost::thread stopping(held.front());
               bool tookFirst = false;
               bool ended = false;

               try
               {
                  tookFirst = eventually([&]() { return resolving(firstKey); });
                  stopping.interrupt();
                  letGo(firstKey);
                  ended = stopping.timed_join(boost::posix_time::seconds(30));
               }
               catch (...)
               {
                  ended = false;
               }

               if (!ended)
               {
                  // Never left running on the bench: let every lookup go and
                  // wait for it, whatever it took.
                  {
                     std::lock_guard<std::mutex> guard(bench.lock);
                     bench.hanging.clear();
                     bench.changed.notify_all();
                  }
                  stopping.join();
               }

               Expect(tookFirst, "The stopping worker never took the first ban.");
               Expect(ended, "The stopping worker did not end once its lookup was answered.");
               Expect(resolvedTimes(secondKey) == 0 && bans.IsPending(second) && announcedCount(secondKey) == 0,
                      "A worker its queue asked to stop took another ban.");
               Expect(announcedCount(firstKey) == 1 && !bans.IsPending(first),
                      "The ban whose answer came in as its worker was asked to stop was not decided by that answer.");

               {
                  std::lock_guard<std::mutex> guard(bench.lock);
                  for (size_t i = 1; i < held.size(); i++)
                     bench.threads.push_back(std::thread(held[i]));
               }
               Expect(eventually([&]() { return announcedCount(secondKey) == 1 && !bans.IsPending(second) && bans.Working() == 0; }),
                      "A worker not asked to stop did not take the waiting ban - the negative control.");
               Expect(announcedCount(firstKey) == 1, "The ban its stopping worker decided was announced again.");
            }

            // A lookup its worker's stop cuts short - the real lookup gives up
            // within a slice of the request, as a stand-in that hangs until it
            // is let go after the interrupt does here - is not an attempt: a ban
            // at its last attempt stays waiting, unannounced, rather than being
            // announced unverified as the service stops. The same ban asked
            // again by a worker not asked to stop, and not answered, is
            // announced (the negative control).
            {
               const IPAddress last = Address("192.0.2.89");
               const String lastKey = CanonicalText(last);

               ban(last, false, 0);
               bans.VerifyBan(last, announcer(lastKey));
               for (int attempt = 1; attempt < NeverBanNameList::MaxAttempts; attempt++)
               {
                  Expect(eventually([&]() { return resolvedTimes(lastKey) == attempt && bans.Working() == 0; }), "An unanswered ban was not asked about again.");
                  Expect(bans.IsPending(last) && announcedCount(lastKey) == 0, "An unanswered ban was announced before its attempts were spent.");

                  advance(NeverBanNameList::RetrySeconds);
                  if (attempt == NeverBanNameList::MaxAttempts - 1)
                  {
                     // Its last attempt, on a worker of the test's own.
                     std::lock_guard<std::mutex> guard(bench.lock);
                     bench.holdPosts = true;
                     bench.hanging.insert(lastKey);
                  }
                  bans.Sweep();
               }

               std::vector<std::function<void()> > held;
               {
                  std::lock_guard<std::mutex> guard(bench.lock);
                  bench.holdPosts = false;
                  held.swap(bench.heldPosts);
               }
               Expect(held.size() == 1, "A ban at its last attempt did not ask for one worker.");

               boost::thread stopping(held.front());
               bool asked = false;
               bool ended = false;

               try
               {
                  asked = eventually([&]() { return resolving(lastKey); });
                  stopping.interrupt();
                  letGo(lastKey);
                  ended = stopping.timed_join(boost::posix_time::seconds(30));
               }
               catch (...)
               {
                  ended = false;
               }

               if (!ended)
               {
                  {
                     std::lock_guard<std::mutex> guard(bench.lock);
                     bench.hanging.clear();
                     bench.changed.notify_all();
                  }
                  stopping.join();
               }

               held.clear();

               Expect(asked && ended, "The stopping worker did not ask about the ban at its last attempt, or did not end.");
               Expect(bans.IsPending(last) && announcedCount(lastKey) == 0 && bans.Working() == 0,
                      "A lookup its worker's stop cut short was counted as an attempt, and the ban at its last attempt announced unverified as the queue stopped.");

               bans.Sweep();
               Expect(eventually([&]() { return announcedCount(lastKey) == 1 && !bans.IsPending(last) && bans.Working() == 0; }),
                      "A ban at its last attempt, asked again by a worker not asked to stop and not answered, was not announced - the negative control.");
               Expect(resolvedTimes(lastKey) == NeverBanNameList::MaxAttempts + 1,
                      "The ban at its last attempt was not asked about once more after the stop.");
            }

            // A worker interrupted inside a lookup or the database - its queue
            // stopping - passes the interruption on, as WorkQueue asks of a
            // task, and leaves the ban waiting as it was, no attempt spent:
            // swallowed, the request to stop would be spent, and the worker
            // would go on taking bans. The next worker takes it at once.
            {
               const IPAddress stopped = Address("192.0.2.88");
               const String stoppedKey = CanonicalText(stopped);

               {
                  std::lock_guard<std::mutex> guard(bench.lock);
                  bench.holdPosts = true;
                  bench.interrupting.insert(stoppedKey);
               }

               ban(stopped, false, 2);
               bans.VerifyBan(stopped, announcer(stoppedKey));

               std::vector<std::function<void()> > held;
               {
                  std::lock_guard<std::mutex> guard(bench.lock);
                  bench.holdPosts = false;
                  held.swap(bench.heldPosts);
               }
               Expect(held.size() == 1, "One waiting ban did not ask for one worker.");

               bool passedOn = false;
               try
               {
                  held.front()();
               }
               catch (const boost::thread_interrupted &)
               {
                  passedOn = true;
               }

               // The work given up, as a stopping queue gives it up: its place
               // is given back.
               held.clear();

               Expect(passedOn, "A worker interrupted inside a lookup swallowed the interruption.");
               Expect(bans.IsPending(stopped) && announcedCount(stoppedKey) == 0 && bans.Working() == 0,
                      "A ban whose worker was interrupted was not left waiting, or its worker's place was not given back.");

               bans.Sweep();
               Expect(eventually([&]() { return announcedCount(stoppedKey) == 1 && !bans.IsPending(stopped) && bans.Working() == 0; }),
                      "A ban whose worker was interrupted was not taken again at once by the next worker - the negative control.");

               // The task the queue runs passes the interruption on, and keeps
               // anything else in.
               bool taskPassedOn = false;
               try
               {
                  NeverBanTask task([]() { throw boost::thread_interrupted(); });
                  task.DoWork();
               }
               catch (const boost::thread_interrupted &)
               {
                  taskPassedOn = true;
               }
               Expect(taskPassedOn, "The verification queue's task swallowed an interruption.");

               bool contained = true;
               try
               {
                  NeverBanTask task([]() { throw std::runtime_error("a failure that is not a stop"); });
                  task.DoWork();
               }
               catch (...)
               {
                  contained = false;
               }
               Expect(contained, "The verification queue's task let a failure out - the negative control.");
            }

            // The crowd: three networks of an outsider, sixteen bans each,
            // whose lookups all hang. Their own seventeenth is announced at
            // once - the cap per network - and a listed provider's ban that
            // comes after them waits, unannounced, and is taken within one
            // lookup of each network, not after all of theirs.
            const char *nets[] = { "198.51.100.", "203.0.113.", "198.18.0." };
            std::vector<String> crowd;
            for (const char *net : nets)
            {
               for (int i = 1; i <= static_cast<int>(NeverBanNameList::MaxPendingPerNetwork); i++)
               {
                  const IPAddress address = Address((std::string(net) + std::to_string(i)).c_str());
                  const String key = CanonicalText(address);
                  ban(address, true, 0);
                  crowd.push_back(key);
                  bans.VerifyBan(address, announcer(key));
               }
            }

            Expect(eventually([&]() { return read([&]() { return bench.resolving.size() == static_cast<size_t>(NeverBanNameList::VerifyThreads); }); }),
                   "The verification threads were not all at work on the crowd.");
            Expect(bans.Count() == 3 * NeverBanNameList::MaxPendingPerNetwork, "The crowd's bans did not all wait.");

            const IPAddress seventeenth = Address("198.51.100.17");
            const String seventeenthKey = CanonicalText(seventeenth);
            ban(seventeenth, true, 0);
            bans.VerifyBan(seventeenth, announcer(seventeenthKey));
            Expect(announcedCount(seventeenthKey) == 1 && !bans.IsPending(seventeenth),
                   "A network's ban past MaxPendingPerNetwork waiting was not announced at once - the negative control of the cap.");

            const IPAddress listed = Address("192.0.2.90");
            const String listedKey = CanonicalText(listed);
            ban(listed, true, 1);
            bans.Hold(listed);
            bans.VerifyBan(listed, announcer(listedKey));
            Expect(bans.IsPending(listed) && announcedCount(listedKey) == 0,
                   "A listed provider's ban was announced while the crowd held every verification thread.");

            int letGoCount = 0;
            while (!resolving(listedKey) && letGoCount < 60)
            {
               String busy;
               {
                  std::lock_guard<std::mutex> guard(bench.lock);
                  for (const String &key : bench.resolving)
                  {
                     if (key != listedKey)
                     {
                        busy = key;
                        break;
                     }
                  }
               }

               Expect(!busy.IsEmpty(), "No verification thread was at work while bans waited.");
               letGo(busy);
               letGoCount++;

               Expect(eventually([&]() { return !resolving(busy) && (resolving(listedKey) || read([&]() { return bench.resolving.size() == static_cast<size_t>(NeverBanNameList::VerifyThreads); })); }),
                      "A thread freed by the crowd took nothing more.");
            }

            Expect(resolving(listedKey), "The listed provider's ban never had its turn.");
            Expect(letGoCount <= 4, "The listed provider's ban waited behind more than one lookup of each network.");

            letGo(listedKey);
            Expect(eventually([&]() { return !bans.IsPending(listed); }), "The listed provider's ban stayed pending after its answer.");
            Expect(wasLifted(listedKey) && announcedCount(listedKey) == 0, "The listed provider's ban was announced, or not lifted, after its turn in the crowd.");

            // A ban that never has its turn - every thread busy - counts a minute
            // past its time as an attempt, and is announced after MaxAttempts of
            // them; still waiting after one fewer.
            const IPAddress starved = Address("192.0.2.91");
            const String starvedKey = CanonicalText(starved);
            ban(starved, false, 0);
            bans.VerifyBan(starved, announcer(starvedKey));
            for (int minute = 1; minute < NeverBanNameList::MaxAttempts; minute++)
            {
               advance(NeverBanNameList::RetrySeconds);
               bans.Sweep();
            }
            Expect(bans.IsPending(starved) && announcedCount(starvedKey) == 0 && resolvedTimes(starvedKey) == 0,
                   "A ban kept from its turn was announced, or looked up, before its attempts were spent - the negative control.");
            advance(NeverBanNameList::RetrySeconds);
            bans.Sweep();
            Expect(announcedCount(starvedKey) == 1 && !bans.IsPending(starved),
                   "A ban kept from its turn for MaxAttempts minutes was not announced.");

            // A ban written while the minute's sweep reads the ranges - held,
            // its range saved and its ban filled after the read began - is not
            // taken for one whose range has gone: the read may simply have
            // missed it. The next sweep reads it and keeps it, and forgets it
            // once its range has really gone (the negative control).
            const IPAddress late = Address("192.0.2.92");
            const String lateKey = CanonicalText(late);
            {
               std::lock_guard<std::mutex> guard(bench.lock);
               bench.duringLive = [&bans, &ban, &announcer, late, lateKey]()
               {
                  bans.Hold(late);
                  ban(late, true, 0);
                  bans.VerifyBan(late, announcer(lateKey));
               };
            }
            bans.Sweep();
            Expect(bans.IsPending(late) && announcedCount(lateKey) == 0,
                   "A ban written while the sweep read the ranges was forgotten as if its range had gone.");
            bans.Sweep();
            Expect(bans.IsPending(late), "A waiting ban whose range the sweep read was forgotten.");
            {
               std::lock_guard<std::mutex> guard(bench.lock);
               bench.live.erase(lateKey);
            }
            bans.Sweep();
            Expect(!bans.IsPending(late) && announcedCount(lateKey) == 0,
                   "A waiting ban whose range had gone by the sweep after it was written was kept, or announced - the negative control.");

            // A full set whose network with the most waiting has nothing that
            // can be given up - its bans are the ones the threads ask about -
            // gives up another network's oldest for a new ban, and does not
            // announce the new one at once: the crowd's last bans hold every
            // thread, and a thousand networks of one ban each fill the rest.
            std::vector<String> filler;
            for (int i = 0; bans.Count() < NeverBanNameList::MaxPending; i++)
            {
               const std::string text = "10." + std::to_string(i / 256) + "." + std::to_string(i % 256) + ".1";
               const IPAddress address = Address(text.c_str());
               const String key = CanonicalText(address);
               ban(address, true, 0);
               filler.push_back(key);
               bans.VerifyBan(address, announcer(key));
               Expect(announcedCount(key) == 0, "A ban was announced at once while the set had room.");
            }
            Expect(bans.Working() == NeverBanNameList::VerifyThreads, "The crowd's last bans did not hold every thread.");

            const IPAddress crowded = Address("192.0.2.93");
            const String crowdedKey = CanonicalText(crowded);
            ban(crowded, true, 1);
            bans.Hold(crowded);
            bans.VerifyBan(crowded, announcer(crowdedKey));
            int givenUp = 0;
            for (const String &key : filler)
               givenUp += announcedCount(key);
            Expect(bans.IsPending(crowded) && announcedCount(crowdedKey) == 0,
                   "A ban was announced at once because the network with the most waiting had none to give up.");
            Expect(givenUp == 1 && bans.Count() == NeverBanNameList::MaxPending,
                   "A full set did not give up exactly one waiting ban for the new one.");

            // A lookup that hangs is given up at its deadline, counted while its
            // thread runs on, and uncounted when it ends; one that answers in
            // time is taken. Counted in a counter of the test's own - static,
            // since a thread given up outlives this frame - so that the
            // service's own verifications, when the class tests run inside it,
            // cannot move the count under the test.
            static std::atomic<int> testAbandoned(0);
            const int abandonedBefore = testAbandoned.load();
            std::vector<String> found;
            const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
            const bool lateAnswered = WithinDeadline([](std::vector<String> &out) -> bool
            {
               std::this_thread::sleep_for(std::chrono::milliseconds(1500));
               out.push_back(_T("late.example.net"));
               return true;
            }, found, 100, testAbandoned);
            const long long tookMs = (long long) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
            Expect(!lateAnswered && found.empty() && tookMs < 1000, "A lookup that hangs was waited for past its deadline.");
            Expect(testAbandoned.load() == abandonedBefore + 1, "A lookup given up was not counted.");

            const bool inTime = WithinDeadline([](std::vector<String> &out) -> bool
            {
               out.push_back(_T("mail5.outbound.example.net"));
               return true;
            }, found, 2000, testAbandoned);
            Expect(inTime && found.size() == 1, "A lookup that answered in time was not taken - the negative control.");
            Expect(eventually([&]() { return testAbandoned.load() == abandonedBefore; }), "A lookup given up stayed counted after its thread ended.");

            // A worker of a queue that is stopping - a boost thread, interrupted
            // - stops waiting for a lookup within a slice, long before the
            // deadline, and counts it given up; a worker not interrupted waits
            // for the answer (the negative control).
            {
               std::atomic<bool> answered(true);
               std::atomic<long long> waitedMs(0);
               boost::thread worker([&answered, &waitedMs]()
               {
                  std::vector<String> out;
                  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
                  answered = WithinDeadline([](std::vector<String> &values) -> bool
                  {
                     std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                     values.push_back(_T("late.example.net"));
                     return true;
                  }, out, 10000, testAbandoned);
                  waitedMs = (long long) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
               });

               std::this_thread::sleep_for(std::chrono::milliseconds(150));
               worker.interrupt();
               worker.join();
               Expect(!answered.load() && waitedMs.load() < 1000, "A worker asked to stop went on waiting for its lookup.");

               boost::thread patient([&answered]()
               {
                  std::vector<String> out;
                  answered = WithinDeadline([](std::vector<String> &values) -> bool
                  {
                     std::this_thread::sleep_for(std::chrono::milliseconds(300));
                     values.push_back(_T("mail5.outbound.example.net"));
                     return true;
                  }, out, 10000, testAbandoned);
               });
               patient.join();
               Expect(answered.load(), "A worker not asked to stop did not wait for its lookup - the negative control.");
               Expect(eventually([&]() { return testAbandoned.load() == abandonedBefore; }), "A lookup given up at a stop stayed counted after its thread ended.");
            }
         }
         catch (const std::logic_error &wrong)
         {
            failure = wrong.what();
         }
         catch (...)
         {
            failure = "NeverBanNameListTester: the waiting bans' bench threw something other than a test failure.";
         }

         // Every thread let go and every ban out of time, whatever happened
         // above, and the threads waited for: nothing of the bench outlives it.
         {
            std::lock_guard<std::mutex> guard(bench.lock);
            bench.hanging.clear();
            bench.clock += static_cast<time_t>(2) * NeverBanNameList::HoldSeconds;
            bench.changed.notify_all();
         }

         for (int round = 0; round < 50; round++)
         {
            bans.Sweep();

            std::vector<std::thread> threads;
            {
               std::lock_guard<std::mutex> guard(bench.lock);
               threads.swap(bench.threads);
            }

            if (threads.empty() && bans.Count() == 0)
               break;

            for (std::thread &thread : threads)
               thread.join();
         }

         if (!failure.empty())
            throw std::logic_error(failure);

         Expect(bans.Count() == 0 && bans.Working() == 0, "Bans or threads were left after every ban ran out of time.");
      }
   }
}
