// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"


#ifndef HM_PLATFORM_POSIX
// Two Windows-only headers, and the reason this file has a POSIX arm at all.
//
// hMailServer.h is the generated COM interface header for the administration API
// - what a script is handed as the "hMailServer" object - and it is produced by
// #import from a type library, which is an MSVC facility. ScriptSite.h is the ATL
// Active Scripting host: IActiveScript, IActiveScriptSite, and the VBScript and
// JScript engines Windows ships. Neither has an equivalent on POSIX.
//
// So the Linux builds run event scripts in a third language, JavaScript, in the
// QuickJS engine the tree vendors (ScriptBinding.h); the class keeps its shape
// and its state on both - the handler flags, the loaded text, the lock that keeps
// them coherent, the all-or-nothing load - and each platform refuses the
// language it cannot run, when the script is loaded, in the error log. What it
// must never do is stay silent: an administrator who carried an EventHandlers.vbs
// across from Windows would otherwise have OnClientLogon and
// OnClientValidatePassword quietly not run, which is a security control
// disappearing without a word. See hmailserver/docs/EventScripts.md.
#include "../../hMailServer/hMailServer.h"
#endif


#include "ScriptServer.h"
#ifndef HM_PLATFORM_POSIX
#include "ScriptSite.h"
#else
#include "ScriptBinding.h"
#endif
#include "ScriptObjectContainer.h"
#include "../Application/RoleManager.h"
#include "../Util/AuditTrail.h"


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
#ifndef HM_PLATFORM_POSIX
      // Aborts a runaway script by interrupting the engine from a timer queue
      // thread.
      //
      // The watchdog holds its own reference to the engine, so the engine object
      // stays alive for as long as the watchdog exists - even after Terminate has
      // released the script site's reference. Disarm blocks until a callback which
      // has already started has returned and guarantees that no further callback
      // will start, and it always runs before the reference is dropped. A callback
      // therefore always executes while the engine is still alive; interrupting an
      // engine which has already been closed simply fails with an HRESULT.
      //
      // InterruptScriptThread is the only IActiveScript method documented as
      // callable from a thread other than the one which created the engine, so it
      // is the only thing the callback is allowed to touch. In particular the
      // callback must not report errors: reporting fires the OnError script event,
      // which would run a script engine on a pool thread that has not initialized
      // COM. The calling thread reports instead, once Disarm has returned.
      class ScriptWatchdog
      {
      public:
         ScriptWatchdog(const CComPtr<IActiveScript> &engine, int timeout_seconds) :
            engine_(engine),
            timer_(0),
            fired_(0)
         {
            // Zero means the administrator has turned the limit off.
            if (timeout_seconds <= 0 || !engine_)
               return;

            DWORD due_time = static_cast<DWORD>(timeout_seconds) * 1000UL;

            if (!CreateTimerQueueTimer(&timer_, NULL, OnTimeout_, this, due_time, 0, WT_EXECUTEONLYONCE | WT_EXECUTELONGFUNCTION))
            {
               DWORD last_error = ::GetLastError();

               timer_ = 0;

               // Logged rather than reported, since reporting fires the OnError
               // event which would arm another watchdog and fail again.
               String sMessage;
               sMessage.Format(_T("ScriptServer: Failed to create the script execution watchdog timer. Windows error code: %d"), last_error);
               Logger::Instance()->LogError(sMessage);
            }
         }

         ~ScriptWatchdog()
         {
            Disarm();
         }

         void Disarm()
         {
            if (timer_ == 0)
               return;

            HANDLE timer = timer_;
            timer_ = 0;

            // INVALID_HANDLE_VALUE makes this wait for a callback which is already
            // running, so once this succeeds the callback can no longer reach this
            // object. It must never be called from the callback itself.
            //
            // The result is checked because a failure is the one path that leaves a
            // callback able to run against a watchdog that is about to be destroyed
            // - a stack object here - so it must not pass silently.
            if (!DeleteTimerQueueTimer(NULL, timer, INVALID_HANDLE_VALUE))
            {
               DWORD deleteError = GetLastError();

               String message;
               message.Format(_T("Could not cancel the script execution watchdog. Windows error code: %d."), (int) deleteError);
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5022, "ScriptServer::ScriptWatchdog::Disarm", message);
            }
         }

         bool Fired()
         {
            return InterlockedCompareExchange(&fired_, 0, 0) != 0;
         }

      private:

         static VOID CALLBACK OnTimeout_(PVOID parameter, BOOLEAN /*timer_or_wait_fired*/)
         {
            ScriptWatchdog *watchdog = reinterpret_cast<ScriptWatchdog*>(parameter);

            InterlockedExchange(&watchdog->fired_, 1);

            watchdog->engine_->InterruptScriptThread(SCRIPTTHREADID_ALL, NULL, 0);
         }

         CComPtr<IActiveScript> engine_;
         HANDLE timer_;
         volatile LONG fired_;
      };
#endif

      // WHO IS ACTING, AROUND A SCRIPT.
      //
      // The audit trail and RoleManager each keep the acting credential in a
      // thread-local, and COM installs both from every authority question it is
      // asked (COMAuthentication::InstallActor_) and never takes them away - on
      // COM's own pool threads the next question simply replaces them. A script
      // is not on COM's threads. It runs in process, on whichever server thread
      // fired the event: an SMTP, IMAP or POP3 worker, a delivery thread, the
      // thread that reloads the scripts. A script that creates
      // hMailServer.Application, authenticates and asks it anything installs
      // that credential on THAT thread, and it stayed there after the script had
      // gone - so the next change the server made on the thread by itself was
      // recorded as the script's credential's. The auto-ban range a failed logon
      // writes, on the very thread whose OnClientValidatePassword just ran, is
      // the plain case: the server banning an address appeared on the audit trail
      // as the administrator doing it.
      //
      // So a script runs inside this. It takes both actors off the thread before
      // the script starts - the script is not whoever happened to be acting on
      // the thread, so nothing it changes without authenticating is recorded
      // under them - lets the script install its own through COM as it always
      // could, so what it changes as an administrator is recorded as that, and
      // puts back exactly what was there before when the script has finished and
      // been torn down: nothing, on a worker thread; the request's credential,
      // when a REST request reloaded the scripts.
      //
      // The JavaScript engine of the Linux builds has no COM to sign in to, so a
      // script there installs no actor of its own; the boundary is kept around it
      // all the same, for the first half of the rule - what a handler changes
      // (Message.Save) is never recorded as whoever happened to be acting on the
      // thread that fired the event.
      class ScriptActorBoundary
      {
      public:
         ScriptActorBoundary() :
            audit_scope_(AuditTrail::Actor()),
            role_actor_(RoleManager::CurrentActor())
         {
            RoleManager::ClearCurrentActor();
         }

         ~ScriptActorBoundary()
         {
            // The role actor here, and the audit trail's as audit_scope_ goes.
            RoleManager::SetCurrentActor(role_actor_);
         }

      private:
         ScriptActorBoundary(const ScriptActorBoundary &);
         ScriptActorBoundary &operator=(const ScriptActorBoundary &);

         // Installs no actor, and restores the one it found when it goes.
         AuditScope audit_scope_;
         RoleActor role_actor_;
      };

#ifdef HM_PLATFORM_POSIX
      // The name the JavaScript engine gives the script in a stack: "at OnHELO
      // (EventHandlers.js:4:7)". The full path is in the report around it.
      const wchar_t *JavaScriptFileName = _T("EventHandlers.js");

      // The objects each event's JavaScript handler is called with, in order - the
      // same parameters, in the same order, as the call text each event's caller
      // builds for VBScript and JScript. The values some events pass after them
      // (a password, a recipient and an error, a remote UID, a backup's failure,
      // OnError's four) are added to the container by the caller; see
      // ScriptArgument. An object the container does not hold is passed as null:
      // OnExternalAccountDownload without a message, and the SMTP events fired
      // before MAIL FROM.
      std::vector<String> JavaScriptObjectArguments_(ScriptServer::Event e)
      {
         std::vector<String> names;

         switch (e)
         {
         case ScriptServer::EventOnClientConnect:
         case ScriptServer::EventOnHELO:
         case ScriptServer::EventOnClientLogon:
            names.push_back(_T("HMAILSERVER_CLIENT"));
            break;
         case ScriptServer::EventOnAcceptMessage:
         case ScriptServer::EventOnSMTPData:
         case ScriptServer::EventOnRecipientUnknown:
         case ScriptServer::EventOnTooManyInvalidCommands:
            names.push_back(_T("HMAILSERVER_CLIENT"));
            names.push_back(_T("HMAILSERVER_MESSAGE"));
            break;
         case ScriptServer::EventOnDeliverMessage:
         case ScriptServer::EventOnDeliveryStart:
         case ScriptServer::EventOnDeliveryFailed:
         case ScriptServer::EventCustom:
            names.push_back(_T("HMAILSERVER_MESSAGE"));
            break;
         case ScriptServer::EventOnExternalAccountDownload:
            names.push_back(_T("HMAILSERVER_FETCHACCOUNT"));
            names.push_back(_T("HMAILSERVER_MESSAGE"));
            break;
         case ScriptServer::EventOnClientValidatePassword:
            names.push_back(_T("HMAILSERVER_ACCOUNT"));
            break;
         case ScriptServer::EventOnBackupCompleted:
         case ScriptServer::EventOnBackupFailed:
         case ScriptServer::EventOnError:
         default:
            break;
         }

         return names;
      }
#endif
   }

   ScriptServer::ScriptServer(void) :
      has_on_client_connect_(false),
      has_on_accept_message_(false),
      has_on_deliver_message_(false),
      has_on_backup_completed_(false),
      has_on_backup_failed_(false),
      has_on_delivery_start_(false),
      has_on_error_(false),
      has_on_delivery_failed_(false),
      has_on_external_account_download_(false),
      has_on_smtpdata_(false),
      has_on_helo_(false),
      has_on_client_logon_(false),
      has_on_client_validate_password_(false),
      has_on_recipient_unknown_(false),
      has_on_too_many_invalid_comands_(false),
      scripts_loaded_(false),
      refusal_reported_(false)
   {
      
   }

   ScriptServer::~ScriptServer(void)
   {
      
   }

   String
   ScriptServer::GetScriptExtension_(const String &language)
   {
      if (language == _T("VBScript"))
         return "vbs";

      if (language == _T("JScript"))
         return "js";

      if (language == _T("JavaScript"))
         return "js";

      // An unrecognized language deliberately gives an empty extension, which
      // yields a file name that will not be found and so a script which is empty.
      return "";
   }

   std::vector<AnsiString>
   ScriptServer::GetRunnableLanguages()
   {
      std::vector<AnsiString> languages;

#ifdef HM_PLATFORM_POSIX
      languages.push_back("JavaScript");
#else
      languages.push_back("VBScript");
      languages.push_back("JScript");
#endif

      return languages;
   }

   bool
   ScriptServer::IsRunnableLanguage_(const String &language)
   {
      std::vector<AnsiString> runnable = GetRunnableLanguages();

      for (const AnsiString &name : runnable)
      {
         if (language == String(name))
            return true;
      }

      return false;
   }

   String
   ScriptServer::GetLoadedLanguage() const
   {
      boost::lock_guard<boost::recursive_mutex> guard(state_mutex_);
      return script_language_;
   }

   bool
   ScriptServer::ClaimRefusalReport_()
   {
      boost::lock_guard<boost::recursive_mutex> guard(state_mutex_);

      if (refusal_reported_)
         return false;

      refusal_reported_ = true;
      return true;
   }

   void
   ScriptServer::ReportRejectedLoad_(const String &sMessage)
   {
      // Reported before anything has been installed, so firing OnError from here
      // runs the previous script, which is still the one in force.
      String report;

      if (scripts_loaded_)
      {
         report = "The scripts in the event directory could not be compiled and have not been loaded. The previously loaded scripts remain in force, so the server is still running the event handlers it had before this reload. ";
         report += sMessage;

         ErrorManager::Instance()->ReportError(ErrorManager::High, 5710, "ScriptServer::LoadScripts", report);
      }
      else
      {
         report = "The scripts in the event directory could not be compiled and have not been loaded. No event handlers are registered until this is fixed and the scripts are reloaded. ";
         report += sMessage;

         ErrorManager::Instance()->ReportError(ErrorManager::High, 5016, "ScriptServer::LoadScripts", report);
      }
   }

   String
   ScriptServer::GetCurrentScriptFile() const
   {
      String sEventsDir = IniFileSettings::Instance()->GetEventDirectory();

      String sScriptLanguage = Configuration::Instance()->GetScriptLanguage();

      String sFileName = FileUtilities::Combine(sEventsDir, "EventHandlers." + GetScriptExtension_(sScriptLanguage));

      return sFileName;
   }

   void
   ScriptServer::LoadScripts()
   {
      // A load is all-or-nothing, and it never partly succeeds.
      //
      // The handler flags gate security-relevant events - OnClientLogon and
      // OnClientValidatePassword among them - so the server must never advertise a
      // handler which the script text it is holding cannot actually run. Nothing is
      // installed here until the candidate script has both compiled and been
      // searched for handlers, so a load which fails leaves the previously loaded
      // script in force in its entirety: contents, language and flags together.
      //
      // Keeping the last known-good script is the conservative choice for a mail
      // server. The alternative - clearing everything when a load fails - would let
      // a typo in a hot reload silently switch off an anti-spam or a logon handler
      // on a running server, which is worse than continuing to run the script that
      // was working a moment earlier. On a cold start there is no known-good script
      // to keep, so the same code path registers nothing at all, which is what
      // fail-closed means there.
      try
      {
         String new_language = Configuration::Instance()->GetScriptLanguage();
         String new_extension = GetScriptExtension_(new_language);

         String script_file = GetCurrentScriptFile();

         // Read once, then compile and search that exact text. Reading the file
         // again for the syntax check would allow a file which changed in between to
         // be checked and a different one installed.
         String new_contents = FileUtilities::ReadCompleteTextFile(script_file);

         String sMessage = CompileContents_(new_language, script_file, new_contents);
         if (!sMessage.IsEmpty())
         {
            ReportRejectedLoad_(sMessage);
            return;
         }

         // Determine which functions are available. Detected into locals: each of
         // these runs the script, so any of them can fail, and committing them as
         // they are found would leave some flags describing the new script and the
         // rest the old one. A search that could not be made at all fails the load,
         // as a compile error does: installing the script with that handler off
         // would switch OnClientValidatePassword or OnClientLogon off in silence.
         String detection_failure;

         auto detect = [&](const String &procedure) -> bool
         {
            if (!detection_failure.IsEmpty())
               return false;

            return DoesFunctionExist_(new_language, new_contents, procedure, detection_failure);
         };

         bool on_client_connect = detect(_T("OnClientConnect"));
         bool on_accept_message = detect(_T("OnAcceptMessage"));
         bool on_deliver_message = detect(_T("OnDeliverMessage"));
         bool on_backup_completed = detect(_T("OnBackupCompleted"));
         bool on_backup_failed = detect(_T("OnBackupFailed"));
         bool on_delivery_start = detect(_T("OnDeliveryStart"));
         bool on_error = detect(_T("OnError"));
         bool on_delivery_failed = detect(_T("OnDeliveryFailed"));
         bool on_external_account_download = detect(_T("OnExternalAccountDownload"));
         bool on_smtpdata = detect(_T("OnSMTPData"));
         bool on_helo = detect(_T("OnHELO"));
         bool on_client_logon = detect(_T("OnClientLogon"));
         bool on_client_validate_password = detect(_T("OnClientValidatePassword"));
         bool on_recipient_unknown = detect(_T("OnRecipientUnknown"));
         bool on_too_many_invalid_commands = detect(_T("OnTooManyInvalidCommands"));

         if (!detection_failure.IsEmpty())
         {
            ReportRejectedLoad_("File: " + script_file + "\r\n" +
               "The script compiled, but a later run of it, made to find out which event handlers it declares, did not complete: " +
               detection_failure);
            return;
         }

         // Commit. Nothing in this block runs a script, reports an error or takes
         // another lock, so an event being fired on another thread sees either the
         // whole of the old script or the whole of the new one.
         boost::lock_guard<boost::recursive_mutex> guard(state_mutex_);

         script_language_ = new_language;
         script_extension_ = new_extension;
         script_contents_ = new_contents;

         // A new script, so a refusal FireEvent makes from here on is news again.
         refusal_reported_ = false;

         has_on_client_connect_ = on_client_connect;
         has_on_accept_message_ = on_accept_message;
         has_on_deliver_message_ = on_deliver_message;
         has_on_backup_completed_ = on_backup_completed;
         has_on_backup_failed_ = on_backup_failed;
         has_on_delivery_start_ = on_delivery_start;
         has_on_error_ = on_error;
         has_on_delivery_failed_ = on_delivery_failed;
         has_on_external_account_download_ = on_external_account_download;
         has_on_smtpdata_ = on_smtpdata;
         has_on_helo_ = on_helo;
         has_on_client_logon_ = on_client_logon;
         has_on_client_validate_password_ = on_client_validate_password;
         has_on_recipient_unknown_ = on_recipient_unknown;
         has_on_too_many_invalid_comands_ = on_too_many_invalid_commands;

         scripts_loaded_ = true;
      }
      catch (...)
      {
         // Nothing is installed before the commit above, and the commit itself only
         // assigns, so there is nothing to roll back here - whatever was loaded
         // before is untouched and still coherent.
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5017, "ScriptServer::LoadScripts", "An exception was thrown when loading scripts. The scripts have not been loaded; any scripts loaded before this attempt remain in force.");
      }
   }


   String
   ScriptServer::CheckSyntax()
   {
      String sEventsDir = IniFileSettings::Instance()->GetEventDirectory();      

      String sScriptLanguage = Configuration::Instance()->GetScriptLanguage();
      String sScriptExtension = GetScriptExtension_(sScriptLanguage);

      String sErrorMessage;

      // Compile the scripts.
      sErrorMessage = Compile_(sScriptLanguage, FileUtilities::Combine(sEventsDir, "EventHandlers." + sScriptExtension));
      if (!sErrorMessage.IsEmpty())
         return sErrorMessage;

      return String("");
   }

#ifndef HM_PLATFORM_POSIX
   bool
   ScriptServer::RunInterruptible_(CComObject<CScriptSiteBasic> *pBasic)
   {
      ScriptWatchdog watchdog(pBasic->GetEngine(), IniFileSettings::Instance()->GetScriptTimeout());

      pBasic->Run();

      // Disarm before reading the flag, so that the callback cannot still be in
      // flight while the caller decides what to do, and so that it can never run
      // concurrently with the Terminate which follows.
      watchdog.Disarm();

      return watchdog.Fired();
   }
#endif

   void
   ScriptServer::ReportInterruption_(const String &sContext, bool bReportError)
   {
      String sMessage;
#ifdef HM_PLATFORM_POSIX
      // The JavaScript engine's interrupt is polled inside the interpreter and the
      // regular expression matcher, and a script cannot catch it; unlike an Active
      // Scripting interrupt there is no COM call it can be stuck behind.
      sMessage.Format(_T("The JavaScript %s did not complete within %d seconds (ScriptTimeout) and was interrupted. The event went on without it."),
         sContext.c_str(), IniFileSettings::Instance()->GetScriptTimeout());
      const int errorCode = 7341;
#else
      sMessage.Format(_T("The script %s did not complete within %d seconds and was interrupted. An interrupt aborts script execution, but cannot release a handler which is blocked inside a COM call."),
         sContext.c_str(), IniFileSettings::Instance()->GetScriptTimeout());
      const int errorCode = 5019;
#endif

      if (bReportError)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, errorCode, "ScriptServer::ReportInterruption_", sMessage);
      }
      else
      {
         // Reporting an error fires the OnError event. When it is the OnError
         // handler itself which had to be killed, firing it again would recurse
         // until the stack is exhausted.
         Logger::Instance()->LogError(sMessage);
      }
   }

   bool
   ScriptServer::DoesFunctionExist_(const String &language, const String &contents, const String &sProcedure, String &sFailure)
   {
#ifdef HM_PLATFORM_POSIX
      // Finding out whether a script declares a procedure means RUNNING the
      // script, as it does on Windows: the whole top level runs once per handler
      // looked for, under the same timeout. For VBScript and JScript there is no
      // engine here, and false is the honest answer and the safe one - a handler
      // that is not registered is never fired, so nothing can believe that
      // OnClientValidatePassword is being consulted when it is not. LoadScripts
      // calls CompileContents_ first, which refuses and reports those languages
      // while scripting is on, so a real script in them never gets this far.
      if (language != _T("JavaScript") || contents.IsEmpty())
         return false;

      ScriptActorBoundary actors;

      ScriptRunOutcome outcome = ScriptBinding::FindFunction(contents, JavaScriptFileName, sProcedure,
         IniFileSettings::Instance()->GetScriptTimeout());

      if (outcome.interrupted)
      {
         // Reported here, as the Windows host reports it; the handler is left
         // unregistered, as it is there.
         ReportInterruption_("EventHandlers (function detection)", true);
      }
      else if (!outcome.completed)
      {
         // The engine could not start, the top level threw on this run although
         // it did not when the script was compiled, or the run reached the memory
         // ceiling, or the lookup itself failed. None of those says whether the
         // handler is there, and "no" would switch it off without a word.
         sFailure = outcome.error.IsEmpty() ? String(_T("the JavaScript engine gave no reason")) : outcome.error;

         if (outcome.out_of_memory)
         {
            String sCeiling;
            sCeiling.Format(_T("The run reached the JavaScript engine's memory ceiling of %d MB. "),
               static_cast<int>(ScriptBinding::MemoryCeiling / (1024 * 1024)));
            sFailure = sCeiling + sFailure;
         }
      }

      return outcome.function_exists;
#else
      // A failure to create the script site is reported below as HM5017, as it
      // always was; the Windows engines have no other way for the search itself
      // to fail that is not their own Script Error line.
      (void) sFailure;

      // JavaScript is the Linux builds' engine. CompileContents_ refuses it here
      // while scripting is on and lets it pass, installing nothing that runs, while
      // scripting is off - so it can reach this point, and must stop before an
      // engine is created: "JavaScript" is a ProgID Windows registers for JScript.
      // The same for any other name that is not one of this build's two.
      if (!IsRunnableLanguage_(language))
         return false;

      // Code outside a function runs here, once per handler looked for, on the
      // thread reloading the scripts - see ScriptActorBoundary.
      ScriptActorBoundary actors;

      // Create an instance of the script engine and execute the script.
      CComObject<CScriptSiteBasic>* pBasic;
      CComObject<CScriptSiteBasic>::CreateInstance(&pBasic);

      CComQIPtr<IActiveScriptSite> spUnk;

      if (!pBasic)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5017, "ScriptServer::FireEvent", "Failed to create instance of script site.");
         return false;
      }

      spUnk = pBasic;
      pBasic->Initiate(language, NULL);
      pBasic->AddScript(contents);
      bool interrupted = RunInterruptible_(pBasic);
      bool bExists = pBasic->ProcedureExists(sProcedure);
      pBasic->Terminate();

      if (interrupted)
      {
         ReportInterruption_("EventHandlers (function detection)", true);
      }

      return bExists;
#endif
   }


   String
   ScriptServer::Compile_(const String &sLanguage, const String &sFilename)
   {
      return CompileContents_(sLanguage, sFilename, FileUtilities::ReadCompleteTextFile(sFilename));
   }

   String
   ScriptServer::CompileContents_(const String &sLanguage, const String &sFilename, const String &sContents)
   {
      // A language this build does not run is refused before the file is looked
      // at, so an empty file - or none at all, which reads as empty - is refused as
      // a full one is. Checking the contents first let a load in such a language
      // through whenever the file for it did not exist yet: an administrator on
      // VBScript who stored JavaScript on Windows (or VBScript on Linux) and
      // reloaded before writing EventHandlers.js had every handler switched off
      // with no line in the ERROR log.
      if (!IsRunnableLanguage_(sLanguage))
      {
         // With scripting switched off, a script file is left alone: no handler
         // would run whether it compiled or not, so nothing is lost by installing
         // nothing, and the report below - which the message tells the operator
         // is stopped by switching scripting off - is not made. LoadScripts runs
         // at every reload regardless of the switch, so without this a file left
         // behind by a test, or by an install migrated from another platform, was
         // reported as an error at every reinitialise for as long as it existed.
         if (!Configuration::Instance()->GetUseScriptServer())
            return "";

#ifdef HM_PLATFORM_POSIX
         // A VBScript or JScript script, and this platform has no engine to compile
         // it in. The caller treats a non-empty return as "the load failed", which
         // is exactly right: nothing is installed, no handler flag is set, and
         // LoadScripts reports the message through ErrorManager - once per load -
         // saying whether a previously loaded script remains in force.
         //
         // Answering "" instead would say the script compiled cleanly, and the
         // server would go on to register handlers that can never run. That is the
         // silent failure this branch exists to prevent - and it would be silent in
         // the worst possible place, since OnClientLogon and OnClientValidatePassword
         // are authentication decisions.
         return "File: " + sFilename + "\r\n" +
            "The event script could not be compiled: this build of hMailServer runs event scripts in JavaScript only, "
            "and the script language is " + sLanguage + ". VBScript and JScript are Windows Active Scripting engines "
            "with no equivalent on this platform, so no event handler in this file will run. Set the script language "
            "to JavaScript and write the handlers in EventHandlers.js (see EventScripts.md), or turn scripting off, "
            "to stop this being reported on every reload.";
#else
         // The setting accepts JavaScript on every platform, so that a backup, an
         // environment configuration or a configuration document made on Linux
         // applies here; the engine is in the Linux builds only. Refused when the
         // script is loaded, as a VBScript file is on Linux - and left alone, as
         // that is, while scripting is switched off. Checked before an engine is
         // created: "JavaScript" is also a ProgID Windows registers for JScript,
         // and CoCreateInstance would otherwise run the file as JScript.
         if (sLanguage == _T("JavaScript"))
         {
            return "File: " + sFilename + "\r\n" +
               "The event script could not be compiled: the script language is JavaScript, the engine the Linux builds "
               "of hMailServer embed. This Windows build runs event scripts in VBScript and JScript through Windows "
               "Active Scripting, so no event handler in this file will run. Set the script language to VBScript or "
               "JScript, or turn scripting off, to stop this being reported on every reload.";
         }

         // Any other word: the setting refuses one, so it can only have been
         // written into the database by hand.
         return "File: " + sFilename + "\r\n" +
            "The event script could not be compiled: the script language is '" + sLanguage + "', which is none of "
            "the three hMailServer knows. This Windows build runs event scripts in VBScript and JScript, so no event "
            "handler will run. Set the script language to VBScript or JScript, or turn scripting off, to stop this "
            "being reported on every reload.";
#endif
      }

      if (sContents.IsEmpty())
         return "";

#ifdef HM_PLATFORM_POSIX
      // JavaScript, the one language this build runs - the check above has turned
      // every other away.
      {
         // Compiling a script means running its top level, here as on Windows, so a
         // top level that throws fails the load as a syntax error does - and one that
         // runs past the timeout is reported the way the Windows host reports it.
         ScriptActorBoundary actors;

         ScriptRunOutcome outcome = ScriptBinding::RunScript(sContents, JavaScriptFileName,
            IniFileSettings::Instance()->GetScriptTimeout());

         if (outcome.completed)
            return "";

         String sErrorMessage = outcome.error;

         if (outcome.interrupted)
         {
            String sTimeoutMessage;
            sTimeoutMessage.Format(_T("The script file %s did not finish executing within %d seconds and was interrupted. Code outside a function runs when the file is loaded, so it must return promptly. The script has not been loaded."),
               sFilename.c_str(), IniFileSettings::Instance()->GetScriptTimeout());

            ErrorManager::Instance()->ReportError(ErrorManager::High, 5021, "ScriptServer::CompileContents_", sTimeoutMessage);

            sErrorMessage = sTimeoutMessage;
         }
         else if (outcome.out_of_memory)
         {
            String sCeilingMessage;
            sCeilingMessage.Format(_T("The script file %s reached the JavaScript engine's memory ceiling of %d MB while it was loaded. "),
               sFilename.c_str(), static_cast<int>(ScriptBinding::MemoryCeiling / (1024 * 1024)));
            sErrorMessage = sCeilingMessage + sErrorMessage;
         }

         return "File: " + sFilename + "\r\n" + sErrorMessage;
      }
#else
      // VBScript or JScript, the two languages this build runs - the check above
      // has turned every other away, JavaScript among them.
      //
      // The whole file runs once here, on the thread checking or reloading the
      // scripts - see ScriptActorBoundary.
      ScriptActorBoundary actors;

      // Create an instance of the script engine and execute the script.
      CComObject<CScriptSiteBasic>* pBasic;
      CComObject<CScriptSiteBasic>::CreateInstance(&pBasic);

      CComQIPtr<IActiveScriptSite> spUnk;

      if (!pBasic)
         return "ScriptServer:: Failed to create instance of script site.";

      spUnk = pBasic; // let CComQIPtr tidy up for us
      pBasic->Initiate(sLanguage, NULL);
      // pBasic->SetObjectContainer(pObjects);
      pBasic->AddScript(sContents);
      bool interrupted = RunInterruptible_(pBasic);
      pBasic->Terminate();

      String sErrorMessage = pBasic->GetLastError();

      if (interrupted)
      {
         // Reported separately, and with the consequence spelled out. This is not a
         // syntax error, but the caller treats any message the same way - it abandons
         // the load, so the new script does not take effect at all. An administrator
         // has to be told that plainly rather than left reading "compile error".
         // Loading deliberately does not continue: discovering the handlers runs the
         // top-level script once per handler, so a script that hangs here would hang
         // another fifteen times. LoadScripts adds which state the server was left
         // in - the previous script, or nothing at all on a cold start.
         String sTimeoutMessage;
         sTimeoutMessage.Format(_T("The script file %s did not finish executing within %d seconds and was interrupted. Code outside a function runs when the file is loaded, so it must return promptly. The script has not been loaded."),
            sFilename.c_str(), IniFileSettings::Instance()->GetScriptTimeout());

         ErrorManager::Instance()->ReportError(ErrorManager::High, 5021, "ScriptServer::CompileContents_", sTimeoutMessage);

         if (!sErrorMessage.IsEmpty())
            sErrorMessage += "\r\n";

         sErrorMessage += sTimeoutMessage;
      }

      if (!sErrorMessage.IsEmpty())
         sErrorMessage = "File: " + sFilename + "\r\n" + sErrorMessage;

      return sErrorMessage;
#endif

   }

   bool
   ScriptServer::IsHandlerRegistered_(Event e, String &event_name) const
   {
      switch (e)
      {
      case EventOnClientConnect:
         if (!has_on_client_connect_)
            return false;
         event_name = _T("OnClientConnect");
         break;
      case EventOnAcceptMessage:
         if (!has_on_accept_message_)
            return false;
         event_name = _T("OnAcceptMessage");
         break;
      case EventOnDeliverMessage:
         if (!has_on_deliver_message_)
            return false;
         event_name = _T("OnDeliverMessage");
         break;
      case EventOnBackupCompleted:
         if (!has_on_backup_completed_)
            return false;
         event_name = _T("OnBackupCompleted");
         break;
      case EventOnBackupFailed:
         if (!has_on_backup_failed_)
            return false;
         event_name = _T("OnBackupFailed");
         break;
      case EventOnError:
         if (!has_on_error_)
            return false;
         event_name = _T("OnError");
         break;
      case EventOnDeliveryStart:
         if (!has_on_delivery_start_)
            return false;
         event_name = _T("OnDeliveryStart");
         break;
      case EventOnDeliveryFailed:
         if (!has_on_delivery_failed_)
            return false;
         event_name = _T("OnDeliveryFailed");
         break;
      case EventOnExternalAccountDownload:
         if (!has_on_external_account_download_)
            return false;
         event_name = _T("OnExternalAccountDownload");
         break;
      case EventOnSMTPData:
         if (!has_on_smtpdata_)
            return false;
         event_name = _T("OnSMTPData");
         break;
      case EventOnHELO:
         if (!has_on_helo_)
            return false;
         event_name = _T("OnHELO");
         break;
      case EventOnClientLogon:
         if (!has_on_client_logon_)
            return false;
         event_name = _T("OnClientLogon");
         break;
      case EventOnClientValidatePassword:
         if (!has_on_client_validate_password_)
            return false;
         event_name = _T("OnClientValidatePassword");
         break;
      case EventOnRecipientUnknown:
         if (!has_on_recipient_unknown_)
            return false;
         event_name = _T("OnRecipientUnknown");
         break;
      case EventOnTooManyInvalidCommands:
         if (!has_on_too_many_invalid_comands_)
            return false;
         event_name = _T("OnTooManyInvalidCommands");
         break;
      case EventCustom:
         // Not gated by a flag: the caller names the procedure to run.
         break;
      default:
         {
            return false;
         }
      }

      return true;
   }

   void
   ScriptServer::FireEvent(Event e,  const String &sEventCaller, std::shared_ptr<ScriptObjectContainer> pObjects, const String &sCallerLanguage)
   {
      if (!Configuration::Instance()->GetUseScriptServer())
         return;

     // JDR: stores the name of the method that is fired in the script. https://www.progressiverobot.com/forum/viewtopic.php?f=2&t=25497
     String event_name = _T("Unknown");

      String script_language;
      String script_contents;

      {
         // The handler flags and the script text are taken together, under the lock,
         // so that a reload running at the same time cannot show this event a flag
         // that came from the new script and the text of the old one - and so that
         // the text is never copied while it is being replaced.
         //
         // The lock is released before the script runs. A script is allowed to call
         // Scripting.Reload, and holding the lock across execution would deadlock
         // the server on the first script that did.
         boost::lock_guard<boost::recursive_mutex> guard(state_mutex_);

         if (!IsHandlerRegistered_(e, event_name))
            return;

         script_language = script_language_;
         script_contents = script_contents_;
      }

      LOG_DEBUG("Executing event " + event_name);

      // The rule action's function is named by the rule; every other handler by
      // its event.
      String handler_name = (e == EventCustom && pObjects) ? pObjects->GetProcedureName() : event_name;

      String sScript;

      // Build the script.
      if (script_language == _T("VBScript"))
         sScript = script_contents + "\r\n\r\n" + "Call " + sEventCaller + "\r\n";
      else if (script_language == _T("JScript"))
         sScript = script_contents + "\r\n\r\n" + sEventCaller + ";\r\n";

#ifdef HM_PLATFORM_POSIX
      // JavaScript takes no call text: see below.
      (void) sCallerLanguage;

      if (script_language == _T("JavaScript"))
      {
         // No source text is built for JavaScript: the handler is looked up by name
         // and called with JS_Call, with the event's objects and the values its
         // caller added to the container (ScriptArgument). The script text still
         // runs in full first, in a fresh engine, as the Windows path's does.
         ScriptActorBoundary actors;

         ScriptRunOutcome outcome = ScriptBinding::CallHandler(script_contents, JavaScriptFileName, handler_name,
            JavaScriptObjectArguments_(e), pObjects, IniFileSettings::Instance()->GetScriptTimeout());

         ReportJavaScriptOutcome_(outcome, handler_name, e != EventOnError);
      }
      else
      {
         // Reached only by a rule's Run function action, which no handler flag
         // gates: a VBScript or JScript script is refused at load while scripting
         // is on (CompileContents_), and one loaded while scripting was off
         // registers no handler, so no other event gets here. Reported rather than
         // ignored, because a rule believed to run a function that does not run is
         // exactly the failure worth a line in the ERROR log - once per load, not
         // once per message the rule matches.
         if (ClaimRefusalReport_())
         {
            ErrorManager::Instance()->ReportError(ErrorManager::High, 5018, "ScriptServer::FireEvent",
               "The event " + handler_name + " was fired, but no JavaScript event script is loaded, so the handler did not run. "
               "This build runs event scripts in JavaScript only: the script language JavaScript, and the file EventHandlers.js. "
               "Events refused for the same reason are not reported again until the scripts are next loaded.");
         }
      }

      (void) sScript;
#else
      // Nothing but the two languages this build runs is ever handed to an engine.
      // JavaScript - which is also a ProgID Windows registers for JScript - and any
      // other name are refused at load while scripting is on, which keeps the
      // previous script in force; a load made while scripting was off installs such
      // a script all the same, with no handler registered, and scripting can then
      // be switched on without a reload. A rule's Run function action is gated by
      // no flag and reaches it, and without this its call would run as JScript.
      // Reported, once per load.
      if (script_language != _T("VBScript") && script_language != _T("JScript"))
      {
         if (ClaimRefusalReport_())
         {
            String loaded = script_language.IsEmpty() ? String(_T("no language")) : script_language;

            ErrorManager::Instance()->ReportError(ErrorManager::High, 5018, "ScriptServer::FireEvent",
               "The event " + handler_name + " was fired, but the event script in force was loaded in " + loaded +
               ", which this Windows build does not run, so the handler did not run. This build runs event scripts in "
               "VBScript and JScript through Windows Active Scripting: set the script language to one of them and reload "
               "the scripts. Events refused for the same reason are not reported again until the scripts are next loaded.");
         }

         return;
      }

      // A call its caller could not build for the language in force - an empty
      // one, which VBScript runs as a bare "Call" and JScript as a bare ";", or a
      // name with no argument list, which JScript reads without calling it. The
      // callers build their call for GetLoadedLanguage(), so this is a caller
      // that did not; never run in silence.
      if (e != EventCustom && (sEventCaller.IsEmpty() || sEventCaller.Find(_T("(")) < 0))
      {
         if (ClaimRefusalReport_())
         {
            ErrorManager::Instance()->ReportError(ErrorManager::High, 5018, "ScriptServer::FireEvent",
               "The event " + handler_name + " was fired, but no call to it could be built for the event script in force, "
               "which is " + script_language + ", so the handler did not run. Events refused for the same reason are not "
               "reported again until the scripts are next loaded.");
         }

         return;
      }

      // A call quoted for the other language: a reload installed a script in the
      // other language between the moment the caller read GetLoadedLanguage() and
      // the snapshot above. VBScript and JScript quote differently, so the text is
      // not a call the script in force should run - a value quoted for one can end
      // its string early in the other. This one event is skipped.
      if (!sCallerLanguage.IsEmpty() && sCallerLanguage != script_language)
      {
         if (ClaimRefusalReport_())
         {
            ErrorManager::Instance()->ReportError(ErrorManager::High, 5018, "ScriptServer::FireEvent",
               "The event " + handler_name + " was fired while the scripts were being reloaded: its call was built for "
               + sCallerLanguage + " and the script now in force is " + script_language + ", so the handler did not run "
               "for this event. Events refused for the same reason are not reported again until the scripts are next loaded.");
         }

         return;
      }

      // The event's handler runs on the thread that fired it - a protocol
      // worker, a delivery thread - and whatever credential the script signs in
      // to COM with must not outlive it there. See ScriptActorBoundary. Declared
      // before the engine so that it is taken down after the engine, and with it
      // every object the script was still holding.
      ScriptActorBoundary actors;

      CComObject<CScriptSiteBasic>* pBasic;
      CComObject<CScriptSiteBasic>::CreateInstance(&pBasic);
      CComQIPtr<IActiveScriptSite> spUnk;
      
      if (!pBasic)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::High, 5018, "ScriptServer::FireEvent", "Failed to create instance of script site.");
         return;
      }
      
      spUnk = pBasic; // let CComQIPtr tidy up for us
      pBasic->Initiate(script_language, NULL);
      pBasic->SetObjectContainer(pObjects);
      pBasic->AddScript(sScript);
      bool interrupted = RunInterruptible_(pBasic);
      pBasic->Terminate();

      if (interrupted)
      {
         ReportInterruption_(event_name, e != EventOnError);
      }
#endif

      LOG_DEBUG("Event completed");
   }

#ifdef HM_PLATFORM_POSIX
   void
   ScriptServer::ReportJavaScriptOutcome_(const ScriptRunOutcome &outcome, const String &sContext, bool bReportError)
   {
      if (outcome.completed)
         return;

      if (outcome.engine_failed)
      {
         String sMessage = "The JavaScript engine could not be started for " + sContext + ", so the handler did not run: the server is short of memory.";

         if (bReportError)
            ErrorManager::Instance()->ReportError(ErrorManager::High, 7340, "ScriptServer::FireEvent", sMessage);
         else
            Logger::Instance()->LogError(sMessage);

         return;
      }

      if (outcome.interrupted)
      {
         ReportInterruption_(sContext, bReportError);
         return;
      }

      if (outcome.out_of_memory)
      {
         // Stopped by the ceiling, not by a bug the script can see: every run has
         // the same 64 MB, and what the handler was doing when it reached it is in
         // the Script Error text that follows.
         String sMessage;
         sMessage.Format(_T("The JavaScript %s was stopped at the engine's memory ceiling of %d MB and did not complete. The event went on without it. %s"),
            sContext.c_str(), static_cast<int>(ScriptBinding::MemoryCeiling / (1024 * 1024)), outcome.error.c_str());

         if (bReportError)
            ErrorManager::Instance()->ReportError(ErrorManager::High, 7342, "ScriptServer::FireEvent", sMessage);
         else
            Logger::Instance()->LogError(sMessage);

         return;
      }

      // An exception the handler did not catch: one "Script Error" line in the
      // ERROR log, as CScriptSiteBasic::HandleScriptError writes on Windows, and the
      // event carries on with whatever Result the handler had set by then.
      Logger::Instance()->LogError(outcome.error);
   }
#endif

}
