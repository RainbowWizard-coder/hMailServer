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
// Windows - failed all 64 rounds under each, every round making eight T's. It
// also holds, as it compiles, that nothing but T can make a Singleton<T> or a
// BusinessObject<T> (below Probe).
//
// No server code is linked: the headers are all there is to test. GitLab CI's
// linux-build job runs every *-tests program the CMake build makes
// (build/ci/linux-build.sh). One line per failed round, and a non-zero exit when
// there is any.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <type_traits>
#include <vector>

#include "../Common/Util/Singleton.h"

// BusinessObject.h is written in the server's dialect, where __int64 is MSVC's
// keyword; hm_platform.h gives it to this platform as this same macro. Defined
// here so that the program still links nothing of the server.
#if !defined(_MSC_VER) && !defined(__int64)
#define __int64 long long
#endif
#include "../Common/BO/BusinessObject.h"

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

   // Only T derives from Singleton<T> (6.3.6): the base's constructor is private
   // and T is its friend. A class that named another type as its T, and so would
   // be handed that type's Instance(), does not compile - which a program cannot
   // show by running - and nothing but T can make the base, which is a trait it
   // can hold. Against the header before 6.3.6, whose implicit constructor was
   // public, this program does not compile.
   static_assert(!std::is_default_constructible<Singleton<Probe<0> > >::value,
                 "Singleton<T> can be made by something that is not T");

   // BusinessObject<T> is the same shape and is held the same way (6.3.6): only
   // T can make the base, so a business object that named another type as its T
   // does not compile. One that names itself is still made and copied as before,
   // and so is a class derived from one. Against the header before 6.3.6 the
   // first assertion fails.
   class Owner : public HM::BusinessObject<Owner>
   {
   };

   class DerivedOwner : public Owner
   {
   };

   static_assert(!std::is_default_constructible<HM::BusinessObject<Owner> >::value,
                 "BusinessObject<T> can be made by something that is not T");
   // One trait to an assertion: two true traits joined by && read to the static
   // analysis (misc-redundant-expression) as the same operand twice.
   static_assert(std::is_default_constructible<Owner>::value,
                 "a business object that names itself can no longer be made");
   static_assert(std::is_copy_constructible<Owner>::value,
                 "a business object that names itself can no longer be copied");
   static_assert(std::is_default_constructible<DerivedOwner>::value,
                 "a class derived from a business object can no longer be made");
   static_assert(std::is_copy_constructible<DerivedOwner>::value,
                 "a class derived from a business object can no longer be copied");

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
