// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// ScriptSite.h is the ATL Active Scripting host - <activscp.h>, <urlmon.h> and
// IActiveScriptSite - and there is no such engine on POSIX. Only the private
// RunInterruptible_ below names a type from it, so every caller of ScriptServer
// can still see this header on Linux. The Linux builds run event scripts in
// JavaScript instead, in the engine ScriptBinding.h embeds.
#ifdef _MSC_VER
#include "ScriptSite.h"
#endif

namespace HM
{
   class ScriptObjectContainer;
   class ScriptRunOutcome;

   class ScriptServer : public Singleton<ScriptServer>
   {
   public:
      
      enum Event
      {
         EventOnClientConnect = 1001,
         EventOnAcceptMessage = 1002,
         EventOnDeliverMessage = 1003,
         EventOnBackupCompleted = 1004,
         EventOnBackupFailed = 1005,
         EventOnDeliveryStart = 1006,
         EventOnError = 1007,
         EventOnDeliveryFailed = 1008,
         
         EventCustom = 1010,
         
         EventOnExternalAccountDownload = 1011,
         EventOnSMTPData = 1012,
         EventOnHELO = 1013,
         EventOnClientLogon = 1014,
         EventOnClientValidatePassword = 1015,
         EventOnRecipientUnknown = 1016,
         EventOnTooManyInvalidCommands = 1017
      };

      ScriptServer(void);
      ~ScriptServer(void);

      // Fires an event (if the script engine has been turned
      // on in hMailAdmin.
      //
      // sCallerLanguage is the language sEventCaller was written for, when the
      // caller quoted values into it - GetLoadedLanguage(), read when the call
      // was built. A reload can install a script in the other language between
      // that read and this call, and a call quoted for VBScript is not a call
      // JScript should run, nor the reverse: such an event is skipped and
      // reported (once per load) rather than run in the wrong language. Empty for
      // a call that names only the handler and its objects.
      void FireEvent(Event e, const String &sEventCaller, std::shared_ptr<ScriptObjectContainer> pObjects, const String &sCallerLanguage = String());

      // The language of the script in force - the one the last successful load
      // installed - which is what FireEvent runs a call in. It is not always the
      // scriptlanguage setting: storing the setting reloads nothing, and a load
      // the build refuses (a language it does not run, a script that does not
      // compile) leaves the previous script, and its language, in force. So a
      // caller that builds its call as source text builds it for this language,
      // never for the setting's. Empty until a script has been loaded.
      String GetLoadedLanguage() const;

      // A value made safe to stand between the quotes of a JScript string literal,
      // '...' or "...", in the call text the Windows engines are handed - a
      // password, a recipient, an error, a remote UID, a backup's reason. Every
      // character that could end the literal early or change what it holds is
      // written as an escape: the backslash, both quotes, CR, LF, U+2028, U+2029
      // and NUL. One pass, so the backslash is escaped before - and never again
      // after - the escapes this function adds; see ScriptServer.cpp for the
      // attack that escaping the quote alone let through.
      static String EscapeForJScriptLiteral(const String &value);

      // The same for VBScript's "...": the quote doubled, which is the only
      // escape VBScript has, and CR, LF and NUL, which no VBScript literal may
      // hold, spliced in as vbCr, vbLf and Chr(0) - so the handler is given the
      // value as it is, where a line break used to make the call a syntax error.
      static String EscapeForVBScriptLiteral(const String &value);

      // The value trimmed of the white space at either end, as String's TrimLeft and
      // TrimRight trim it, but never into a CR LF pair: what is before the first pair
      // and after the last is trimmed, and everything from the first pair to the last
      // is kept. It is how a VBScript OnDeliveryFailed has always been handed its
      // error - the call joined each pair in before it trimmed, so no trim reached
      // one - and so it keeps the CR LF that ends the error's last line.
      static String TrimKeepingCrLfPairs(const String &value);

      // Whether a rule's "Run function" name can be written into call text as it
      // is: ASCII letters, digits, _ and $, not starting with a digit, at most 255
      // characters - the names the JavaScript engine looks up. Anything else is
      // refused by FireEvent before a Windows engine sees it.
      static bool IsPlainProcedureName(const String &name);

      // The milliseconds the Windows engines' watchdog is armed for, for a
      // ScriptTimeout of timeout_seconds: 0, no watchdog, for zero or less, and
      // otherwise the seconds clamped to 4,294,967 before they are made
      // milliseconds - the longest whole number of seconds the 32-bit count
      // CreateTimerQueueTimer takes can hold. Unclamped, the product wrapped: a
      // ScriptTimeout of 4,294,968 armed a 704 ms watchdog, which ended every
      // event script (release review finding CP-5). The Control Panel holds the
      // setting to the same bound.
      static unsigned long WatchdogMilliseconds(int timeout_seconds);

      // Checks the syntax of the scripts in the
      // event directory and return the result.
      String CheckSyntax();

      // Refreshes the scripts in the event directory. With scripting switched off
      // no engine runs: the script's text and language are installed with no
      // handler registered, and switching scripting on (Configuration::
      // SetUseScriptServer) loads it again, compiled and searched. A load that
      // read the switch as off and finds scripting switched on since installs
      // nothing and is made again, from the switch (switch_generation_); and a
      // load installs nothing over the script of a load that started after it
      // (load_ticket_), so of two loads that overlap, the script of the one
      // started last stays in force - unless it was refused, and said so.
      void LoadScripts();

      // Every switch of scripting to on, for Configuration::SetUseScriptServer:
      // store stores the switch. When the switch reads off, the script is loaded
      // first as a load with scripting on loads it - compiled, searched for its
      // handlers, refused and reported if it does not compile - so that the
      // handlers are registered from the moment an event can see the switch on.
      // After store - whether or not the switch read on already - a load that read
      // the switch as off before it cannot install its uncompiled text any more
      // (switch_generation_), and if one installed it in between, the script is
      // loaded once more (LoadScripts). store may throw
      // (EnvironmentSettings::Refusal, a setting the environment manages); the
      // switch then stays as it was, and the exception is the caller's.
      void SwitchOn(const std::function<void()> &store);
      
      String GetCurrentScriptFile() const;

      // The script languages this build runs, spelled as the scriptlanguage setting
      // stores them: VBScript and JScript on Windows (Active Scripting), JavaScript
      // on the Linux builds (the embedded engine). The setting accepts all three on
      // both, because a backup, an environment configuration and a configuration
      // document (hmctl config export) carry settings between machines; a language
      // the build does not run is refused when the script is loaded, in the error
      // log, once per load. GET /api/v1/status reports this list as
      // build.script_languages.
      static std::vector<AnsiString> GetRunnableLanguages();

      // Whether an event script is running on the calling thread: a handler, a
      // rule's Run function, or the file's top level while it is checked or
      // loaded - every run the server makes, each inside its ScriptActorBoundary.
      // An hMailServer object a Windows script creates lives in this process's
      // MTA, where the event threads are too, so the script calls it directly and
      // its members run on the thread running the script; a COM client in
      // another process is answered on one of COM's own threads, which never run
      // one. InterfaceUtilities::MayUseServerHelpers_ asks it, and so does
      // Application::TakeServersLock_, which waits a few seconds at most for a
      // script's stop, start or reinitialise.
      static bool IsScriptRunningOnThisThread();

      // The calling thread counted as running an event script for as long as
      // this lives, as every run the server makes is counted: for a test that
      // stands in for a script's call (ServersLockTester).
      class ScriptRunOnThisThread
      {
      public:
         ScriptRunOnThisThread();
         ~ScriptRunOnThisThread();

         ScriptRunOnThisThread(const ScriptRunOnThisThread &) = delete;
         ScriptRunOnThisThread &operator=(const ScriptRunOnThisThread &) = delete;
      };

   private:

      static String GetScriptExtension_(const String &language);
      // Returns the file extension used by the given script language, or an empty
      // string if the language is not one hMailServer knows. JScript and
      // JavaScript share "js": a JScript file carried to Linux is the file the
      // JavaScript engine is given, which is how a port starts.

      bool DoesFunctionExist_(const String &language, const String &contents, const String &sProcedure, String &sFailure);
      // Runs the given script text and reports whether it declares the named
      // procedure. Takes the text as an argument rather than reading the loaded
      // script, so that LoadScripts can search a candidate script without having
      // installed it. sFailure is set when the search itself could not be made -
      // on the JavaScript engine: the engine could not start, the top level threw
      // or reached the memory ceiling on this run - and LoadScripts then refuses
      // the whole load, as it refuses a script that does not compile, rather than
      // install the script with that handler quietly switched off.

      static bool IsRunnableLanguage_(const String &language);
      // Whether GetRunnableLanguages names the language.

      void ReportRejectedLoad_(const String &sMessage);
      // Reports a load that was refused - HM5710 when a script loaded earlier stays
      // in force, HM5016 when none has ever been loaded - with sMessage after it.

      bool ClaimRefusalReport_();
      // True the first time it is asked after each successful load, false after:
      // an event FireEvent refuses to run is reported once per load, not once per
      // event, so that a server firing thousands of them writes one line.

      bool IsHandlerRegistered_(Event e, String &event_name) const;
      // Returns whether a handler for e was found in the loaded script, and names
      // the event. Must be called with state_mutex_ held, since it reads the
      // handler flags.

#ifdef _MSC_VER
      bool RunInterruptible_(CComObject<CScriptSiteBasic> *pBasic);
      // Runs the script which has been added to pBasic, under a watchdog which
      // aborts execution once the configured script timeout has elapsed. A
      // timeout of zero means the administrator has disabled the limit.
      // Returns true if the watchdog had to interrupt the script.
#endif

      void ReportInterruption_(const String &sContext, bool bReportError);
      // Reports that the script named by sContext was killed by the watchdog.
      // bReportError must be false when the killed script is the OnError
      // handler itself, since reporting an error fires OnError again.

#ifdef HM_PLATFORM_POSIX
      void ReportJavaScriptOutcome_(const ScriptRunOutcome &outcome, const String &sContext, bool bReportError);
      // Reports what went wrong in a run of the JavaScript engine, if anything:
      // the timeout and the memory ceiling as errors of their own (with the same
      // OnError caveat as above), and an uncaught exception as the one "Script
      // Error" line the Windows host writes, after which the event carries on.
#endif

      String Compile_(const String &sLanguage, const String &sFilename);
      // Compiels the script in sFileName and returns the result. If
      // no compilation errors exists, the function returns an emtpy string.

      String CompileContents_(const String &sLanguage, const String &sFilename, const String &sContents, bool scriptingOn);
      // As Compile_, but compiles text which the caller has already read. sFilename
      // is used only to name the file in the error message. This is what lets a
      // load check exactly the text it is about to install, rather than reading the
      // file a second time and checking whatever is on disk by then. scriptingOn:
      // whether to judge as scripting on does - a language this build does not run
      // is then refused - or as scripting off does, which lets it pass.

      // What a load did.
      struct LoadOutcome
      {
         // The load installed its script: it is the one in force.
         bool committed;

         // A load with scripting off found scripting switched on since it read the
         // switch, and installed nothing: LoadScripts makes it again.
         bool switched_on;

         // loads_committed_ as the commit left it, when committed.
         unsigned long long count;
      };

      LoadOutcome LoadScripts_(bool scriptingOn, unsigned long long generation, unsigned long long ticket);
      // LoadScripts and SwitchOn: a load with scripting off (no engine, nothing
      // registered) or on (compiled and searched). generation, for a load with
      // scripting off, is switch_generation_ as it was read before the switch
      // was; ticket is the load's own, taken before it read anything
      // (load_ticket_).

      bool has_on_client_connect_;
      bool has_on_accept_message_;
      bool has_on_deliver_message_;
      bool has_on_backup_completed_;
      bool has_on_backup_failed_;
      bool has_on_delivery_start_;
      bool has_on_error_;
      bool has_on_delivery_failed_;
      bool has_on_external_account_download_;
      bool has_on_smtpdata_;
      bool has_on_helo_;
      bool has_on_client_logon_;
      bool has_on_client_validate_password_;
      bool has_on_recipient_unknown_;
      bool has_on_too_many_invalid_comands_;

      // Whether a script has ever been loaded successfully. Used only to tell an
      // administrator whether a rejected load left a working script in force or
      // left the server with no event handlers at all.
      bool scripts_loaded_;

      // Whether the script in force was compiled and searched for its handlers:
      // true after a load made with scripting on, false after one made with it
      // off, which runs no engine and registers no handler. A rule's Run function
      // action, which no handler flag gates, runs only a compiled script, so a
      // text that no load has compiled - the load when scripting was switched on
      // refused it - is never handed to an engine. Guarded by state_mutex_.
      bool script_compiled_;

      // Whether FireEvent has reported refusing an event since the last successful
      // load; see ClaimRefusalReport_. Guarded by state_mutex_.
      bool refusal_reported_;

      // How many loads have installed a script, with scripting on or off. SwitchOn
      // compares it with the count its own load left - or, when its load installed
      // nothing, with the count from before it - to see whether another load
      // installed a script since. Guarded by state_mutex_.
      unsigned long long loads_committed_;

      // Raised by SwitchOn once the switch is stored, every time scripting is set
      // on. A load with scripting off reads it before it reads the switch, and does
      // not install its uncompiled text if it has moved by the time it would:
      // scripting was switched on after that load read it as off. LoadScripts then
      // makes the load again, and the switch now reads on, so a reload asked for
      // beside a switch-on is not lost. Without it a reload or a reinitialise that
      // read the switch as off just before SetUseScriptServer stored it on could
      // install its text after the switch's load, leaving scripting on with no
      // handler registered until the next load. Guarded by state_mutex_.
      unsigned long long switch_generation_;

      // The last ticket handed to a load: each load takes the next, with the
      // generation, before it reads the switch or the file. ticket_in_force_ is the
      // ticket of the load whose script is in force, and a load whose ticket is not
      // later than it installs nothing: two loads that overlap - a reload beside a
      // reinitialise, or a switch-on's load that read the file before a reload that
      // finished first - used to install in the order they finished, so the text a
      // load read before an edit could replace the text a later load read after it.
      // A load that started after the edit is always the later one. Both guarded by
      // state_mutex_.
      unsigned long long load_ticket_;
      unsigned long long ticket_in_force_;

      String script_contents_;
      String script_extension_;
      String script_language_;

      // Guards the handler flags and the script text against a reload which runs
      // while events are being fired. The two must be read together: a flag that
      // came from the new script paired with the text of the old one is exactly the
      // incoherence this lock exists to prevent. It is held only long enough to
      // read or to replace them - never while a script runs, since a script is
      // allowed to call Scripting.Reload. Mutable so that GetLoadedLanguage, a
      // read, can take it.
      mutable boost::recursive_mutex state_mutex_;

   };

   // The helpers above, held to examples: what a password that tried to end its
   // string becomes, each escape, the VBScript splices, what the VBScript
   // OnDeliveryFailed's trim keeps, and the procedure names FireEvent lets
   // through. ClassTester::DoTests runs it.
   class ScriptLiteralTester
   {
   public:
      void Test();
   };

   // ScriptServer::WatchdogMilliseconds held to the bound: no watchdog for zero
   // or less, the seconds as milliseconds up to 4,294,967 of them, and that and
   // no less for anything longer. ClassTester::DoTests runs it on Windows and
   // script-binding-tests on the POSIX build.
   class ScriptWatchdogTester
   {
   public:
      void Test();
   };
}