// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The JavaScript event-script engine: QuickJS (hmailserver/source/Server/quickjs,
// Fabrice Bellard's release 2025-04-26) and the binding of the server's own objects
// into it. See hmailserver/docs/EventScripts.md.
//
// POSIX ONLY. The release has no MSVC support, so the engine is built by the CMake
// build alone and this file and ScriptBinding.cpp are in no Visual Studio project;
// on Windows the event scripts are VBScript and JScript through Active Scripting
// (ScriptSite.h), and a script language of JavaScript is refused there when the
// script is loaded. The same way IniFile.cpp is the POSIX build's and not the
// Windows project's.
//
// What a run is. Every call below makes a runtime and a context of its own, parses
// the whole script, does its work and frees both before it returns - the shape the
// Windows path has, where FireEvent creates a fresh engine for every event. Nothing
// the script makes outlives the call, so a script that keeps an object in a global
// cannot reach a C++ object that has since gone, and two threads firing events at
// once never share an engine. What a run is allowed:
//
//   - the language and its standard library, and nothing else: quickjs-libc, where
//     QuickJS's std and os modules live, is not in the tree, no module loader is
//     installed (import() is refused), and Atomics.wait may not block;
//   - ScriptTimeout seconds of execution (a [Settings] key in the settings
//     store, default 60, 0 for no limit), enforced by the engine's interrupt
//     handler, which it polls in the interpreter loop - at calls and branches,
//     every ten thousand steps - and in the regular expression matcher alike, and
//     which a try/catch cannot swallow. What it does NOT stop is one call into a
//     built-in that loops in C without polling: includes, indexOf, lastIndexOf and
//     their kin over an array-like whose length is huge
//     (Array.prototype.includes.call({length: 2**53 - 1}, 1)) run to their end,
//     however long that is, and allocate nothing the ceiling below would stop;
//   - MemoryCeiling bytes of allocation, a constant;
//   - StackCeiling bytes of the calling thread's stack.
#ifdef HM_PLATFORM_POSIX

#include "ScriptObjectContainer.h"

namespace HM
{
   // What one run of the engine came to.
   class ScriptRunOutcome
   {
   public:
      ScriptRunOutcome();

      // The script - and the handler, when one was called - ran to the end with no
      // uncaught exception.
      bool completed;

      // Stopped at the script timeout.
      bool interrupted;

      // Stopped because an allocation was refused at MemoryCeiling.
      bool out_of_memory;

      // The runtime or the context could not be made - memory, in practice.
      bool engine_failed;

      // FindFunction: the name is a function. CallHandler: the handler was found.
      bool function_exists;

      // The line of the first error, from 1, or 0 when there is none or the engine
      // did not say.
      int error_line;

      // One "Script Error: ..." line describing the uncaught exception, with its
      // line and its stack; empty when the run completed.
      String error;

      // What the handler returned, when it returned a string. The server ignores
      // it, as it ignores a VBScript function's value; ScriptBindingTester reads
      // its answers through it.
      String returned;
   };

   class ScriptBinding
   {
   public:
      // The most one run may allocate: 64 MB.
      static constexpr size_t MemoryCeiling = 64 * 1024 * 1024;

      // How much of the calling thread's stack the engine may use.
      static constexpr size_t StackCeiling = 512 * 1024;

      // Compiles the script and runs its top level with no objects bound - what
      // ScriptServer::CompileContents_ does on Windows, where compiling a script
      // means running it. A syntax error, an uncaught exception at the top level
      // and a top level that runs past the timeout all fail it.
      static ScriptRunOutcome RunScript(const String &script, const String &fileName, int timeoutSeconds);

      // Runs the script's top level and reports whether functionName names a
      // function - a function declaration, or a var, let, const or class of that
      // name holding one.
      static ScriptRunOutcome FindFunction(const String &script, const String &fileName, const String &functionName, int timeoutSeconds);

      // Binds the container's objects as globals under their names (EventLog,
      // Result, HMAILSERVER_MESSAGE, ...), runs the script's top level, and calls
      // functionName with the arguments: first the named objects, in order, each
      // the same object as the global of that name and null when the container has
      // none by that name (an event without a message); then the container's own
      // values (ScriptObjectContainer::GetArguments). Promise jobs the handler
      // queues are run before it returns, inside the same timeout.
      static ScriptRunOutcome CallHandler(const String &script, const String &fileName, const String &functionName,
         const std::vector<String> &objectArguments, std::shared_ptr<ScriptObjectContainer> objects, int timeoutSeconds);

      // One member of an object a handler is given, as the binding defines it and
      // as hmailserver/docs/EventScripts.md lists it.
      class Member
      {
      public:
         const char *object;   // Message, Client, Result, EventLog, FetchAccount, Account, Recipients, Recipient
         const char *name;
         const char *kind;     // "read", "read/write" or "method"
      };

      // Every member the binding defines, and every script-visible member of the
      // COM interfaces behind those objects that it deliberately does not.
      static std::vector<Member> Contract();
      static std::vector<Member> NotOnJavaScript();
   };

   // The binding's contract, tested inside the process: every member Contract()
   // names is on the object a handler is given, with its kind, and nothing else is;
   // every member NotOnJavaScript() names is absent; values reach the C++ objects
   // and back; and the limits hold - a loop and a runaway regular expression are
   // interrupted, an allocation past the ceiling is stopped, the std and os modules
   // and import() are not there, a syntax error names its line. Needs no database
   // and no network. Run by the bench program script-binding-tests, which the
   // CMake build makes beside spf-tests and GitLab CI's linux-build job runs;
   // nothing inside the server runs it (ClassTester::DoTests is reached only
   // through COM and the Windows /Test switch, and the engine is not on Windows),
   // and neither regression suite runs the program.
   class ScriptBindingTester
   {
   public:
      void Test();

   private:
      void TestContract_();
      void TestValues_();
      void TestLimits_();
      void TestSandbox_();
      void TestErrors_();
      void TestLanguage_();
   };
}

#endif
