// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

template <class T>
class Singleton
{
public:

   virtual ~Singleton()
   {

   }

   // The one T, made by whichever thread asks first. A function-local static is
   // initialised exactly once however many threads arrive together: the others
   // wait until the constructor has returned, and every caller then sees the
   // whole object (C++11 [stmt.dcl]/4 - MSVC since Visual Studio 2015 unless
   // /Zc:threadSafeInit- is given, GCC and clang unless -fno-threadsafe-statics
   // is; no build of this server gives either). Until 6.3.6 the pointer was
   // tested outside the lock and not again inside it, so two threads reaching a
   // singleton's first Instance() together - several are first reached on a
   // delivery, COM or REST thread rather than at start - could each make a T,
   // and a reader could see the pointer before the object it points at.
   //
   // The T is made with new and never deleted, as it always was: no singleton is
   // destroyed at exit, so none can be used after it is gone by another's
   // destructor or by a thread still running when the process ends.
   static T * Instance()
   {
      static T * const instance = new T();

      return instance;
   }
};