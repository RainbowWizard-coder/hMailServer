// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Singleton<T>::Instance (Common/Util/Singleton.h) under contention, as a program
// of its own. Every one of the server's sixty-odd singletons is made on its first
// Instance() call, and some are first reached on a delivery, COM or REST thread
// rather than at start, so two threads can arrive together. Until 6.3.6 the
// pointer was tested outside the lock and not again inside it, and both could make
// a T; this holds the header to making exactly one, and to every caller seeing it
// whole.
//
// Each round is a fresh singleton type, whose constructor counts itself and then
// sleeps a little so that the other threads arrive while it runs. Eight threads
// wait on one flag and then ask for the instance together. A round passes when the
// constructor ran once, every thread got the same pointer, and every thread saw
// the value the constructor set last. The same program built against the old
// header on 29 September 2026 - GCC 15 and clang 21 on Ubuntu, MSVC 19.51 on
// Windows - failed all 64 rounds under each, every round making eight T's.
//
// No server code is linked: the header is all there is to test. GitLab CI's
// linux-build job runs every *-tests program the CMake build makes
// (build/ci/linux-build.sh). One line per failed round, and a non-zero exit when
// there is any.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "../Common/Util/Singleton.h"

namespace
{
   const int kThreads = 8;
   const int kRounds = 64;
   const int kReady = 0x5eed;

   std::atomic<int> constructions[kRounds];

   template <int N>
   class Probe : public Singleton<Probe<N> >
   {
   public:
      Probe()
      {
         constructions[N]++;
         std::this_thread::sleep_for(std::chrono::milliseconds(2));
         ready = kReady;
      }

      int ready = 0;
   };

   template <int N>
   bool Round()
   {
      std::atomic<bool> go(false);
      std::atomic<int> waiting(0);
      std::vector<Probe<N> *> seen(kThreads, nullptr);
      std::vector<int> ready(kThreads, 0);
      std::vector<std::thread> threads;

      for (int i = 0; i < kThreads; i++)
      {
         threads.emplace_back([&, i]()
         {
            waiting++;
            while (!go.load())
               std::this_thread::yield();

            Probe<N> *instance = Probe<N>::Instance();
            seen[i] = instance;
            ready[i] = instance->ready;
         });
      }

      while (waiting.load() < kThreads)
         std::this_thread::yield();
      go = true;

      for (auto &thread : threads)
         thread.join();

      bool ok = true;
      const int made = constructions[N].load();
      if (made != 1)
      {
         printf("FAIL  round %d: the constructor ran %d times\n", N, made);
         ok = false;
      }

      for (int i = 0; i < kThreads; i++)
      {
         if (seen[i] != seen[0])
         {
            printf("FAIL  round %d: thread %d got another instance\n", N, i);
            ok = false;
            break;
         }
      }

      for (int i = 0; i < kThreads; i++)
      {
         if (ready[i] != kReady)
         {
            printf("FAIL  round %d: thread %d saw the instance before its constructor finished\n", N, i);
            ok = false;
            break;
         }
      }

      // Asked again, on this thread, after everything above: the same one, and no
      // second construction.
      if (Probe<N>::Instance() != seen[0] || constructions[N].load() != made)
      {
         printf("FAIL  round %d: a later Instance() did not return the first instance\n", N);
         ok = false;
      }

      return ok;
   }

   // Rounds 0 to N - 1, in that order: how many failed. A recursion on the round's
   // number, not an array of the rounds' functions filled from an index_sequence,
   // which cppcheck 2.19 - the static-analysis job's - reads as an uninitialised
   // variable (legacyUninitvar): a finding not in the baseline, which the release's
   // security gate (build/ci/security-gate.py) holds a release for.
   template <int N>
   struct Rounds
   {
      static int Failed()
      {
         const int before = Rounds<N - 1>::Failed();
         return before + (Round<N - 1>() ? 0 : 1);
      }
   };

   template <>
   struct Rounds<0>
   {
      static int Failed()
      {
         return 0;
      }
   };
}

int
main(int /*argc*/, char * /*argv*/[])
{
   const int failed = Rounds<kRounds>::Failed();

   if (failed == 0)
   {
      printf("ok    Singleton<T>::Instance: %d rounds of %d threads, one instance each, seen whole\n", kRounds, kThreads);
      return 0;
   }

   printf("FAIL  Singleton<T>::Instance: %d of %d rounds\n", failed, kRounds);
   return 1;
}
