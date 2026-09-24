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

      // Checks the syntax of the scripts in the
      // event directory and return the result.
      String CheckSyntax();

      // Refreshes the scripts in the event directory
      void LoadScripts();
      
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

      String CompileContents_(const String &sLanguage, const String &sFilename, const String &sContents);
      // As Compile_, but compiles text which the caller has already read. sFilename
      // is used only to name the file in the error message. This is what lets a
      // load check exactly the text it is about to install, rather than reading the
      // file a second time and checking whatever is on disk by then.

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

      // Whether FireEvent has reported refusing an event since the last successful
      // load; see ClaimRefusalReport_. Guarded by state_mutex_.
      bool refusal_reported_;

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
}