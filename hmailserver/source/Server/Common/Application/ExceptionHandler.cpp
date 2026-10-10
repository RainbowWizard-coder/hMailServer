// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "ExceptionHandler.h"
#include "../Util/ExceptionLogger.h"

#include <boost/thread/thread.hpp>

#ifdef __GLIBCXX__
#include <cxxabi.h>
#endif


#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   ExceptionHandler::ExceptionHandler(void)
   {

   }

#ifndef HM_PLATFORM_POSIX
   LONG WINAPI ExceptionFilterWithLogging(EXCEPTION_POINTERS* pExp, DWORD dwExpCode)
   {
      // if an error occurs when shutting down, we want to log it completely before
      // the shut down completes.
      boost::this_thread::disable_interruption shutdown_temporarily_disabled;

      LOG_DEBUG("Logging exception..");

      ExceptionLogger::Log(dwExpCode, pExp);

      LOG_DEBUG("Completed logging of exception...");

      return EXCEPTION_EXECUTE_HANDLER;
   }
#endif
   
   
   bool
   ExceptionHandler::Run(const String &descriptive_name, boost::function<void()>& func)
   {
#ifdef HM_PLATFORM_POSIX
      // The C++ half of this function is Windows' own; the hardware half is
      // absent rather than stubbed, and that is the whole of the difference.
      //
      // An exception the task throws is caught, reported and rethrown by
      // RunWithStandardExceptions, and the rethrow ends here, in a false return,
      // as the __except below ends it on Windows - which catches a C++ exception
      // as it catches any other, writes the minidump and answers false. Until 10
      // October 2026 nothing caught it here: it left the function, and on a
      // thread of the server's own - the message indexer's, the SQL log device's,
      // a migration job's - that is std::terminate and the end of the process.
      // The indexer's thread did that in the Linux suite on Alpine, its HM4208
      // the last line of the log ("boost unique_lock has no mutex", a database
      // call made after the database manager was gone; MessageIndexer::Stop). Every
      // caller is written for the false return already, since it is what Windows
      // gives: the work queues keep the worker, the IOCP worker runs its context
      // again, the indexer and the other singleton workers end their thread and
      // leave the process running.
      //
      // __try/__except is an MSVC extension, and there is no POSIX equivalent to
      // guard a call with: a fault arrives here as a signal, not as something a
      // filter expression can inspect and swallow. So an access violation or a
      // divide by zero ends the process instead of being logged and turned into a
      // false return. That is not a silent no-op - a false return on Windows means
      // "the task faulted and the fault has been recorded", and here a task that
      // faults does not return at all - and an administrator is told once at
      // start-up by CrashOracle::LogInstallationStatus rather than being left to
      // assume the server is catching its own faults. Closing the gap needs a
      // signal handler and a core-dump policy, which is the roadmap row "The Win32
      // tail".
      try
      {
         RunWithStandardExceptions(descriptive_name, func);
         return true;
      }
#ifdef __GLIBCXX__
      catch (abi::__forced_unwind &)
      {
         // A thread being cancelled unwinds with this, and it must go on: a
         // forced unwind that is caught and not passed on aborts the process.
         // The server cancels no thread; a library it calls might.
         throw;
      }
#endif
      catch (...)
      {
         // Reported already, by RunWithStandardExceptions.
         return false;
      }
#else
      __try
      {
         RunWithStandardExceptions(descriptive_name, func);
         return true;
      }
      __except (ExceptionFilterWithLogging(GetExceptionInformation(), GetExceptionCode()))
      {
         // this has been logged in the exception filter.
         return false;
      }   
#endif
   }

   void
   ExceptionHandler::RunWithStandardExceptions(const String &descriptive_name, boost::function<void()>& func)
   {
      try
      {
         func();
      }
      catch (boost::thread_interrupted&)
      {
         // shutting down
      }
      catch (boost::system::system_error& error)
      {
         try
         {
            ErrorManager::Instance()->ReportError(ErrorManager::High, 4208, "ExceptionHandler::Run", GetExceptionText(descriptive_name), error);
         }
         catch (...)
         {
            // Don't swallow the original exception.
         }

         throw;
      }
#ifndef HM_PLATFORM_POSIX
      catch (_com_error& error)
      {
         // What the ADO wrappers throw, and not a std::exception: caught below
         // as "..." it was reported with no description at all, and the minidump
         // that followed was the only word on what had failed.
         try
         {
            String hresult;
            hresult.Format(_T("0x%08X"), (unsigned int) error.Error());
            _bstr_t description(error.Description());
            LPCSTR text = description;
            ErrorManager::Instance()->ReportError(ErrorManager::High, 4208, "ExceptionHandler::Run",
               GetExceptionText(descriptive_name) + ", COM error " + hresult + ": " + String(text ? text : ""));
         }
         catch (...)
         {
            // Don't swallow the original exception.
         }
         throw;
      }
#endif
      catch (std::exception& error)
      {
         String sErrorMessage = 
            Formatter::Format("An error occured while executing '{0}'", descriptive_name);

         try
         {
            ErrorManager::Instance()->ReportError(ErrorManager::High, 4208, "ExceptionHandler::Run", GetExceptionText(descriptive_name), error);
         }
         catch (...)
         {
            // Don't swallow the origial exception
         }
         
         throw;
      }
      catch (...)
      {
         String sErrorMessage = 
            Formatter::Format("An error occured while executing '{0}'", descriptive_name);

         try
         {
            ErrorManager::Instance()->ReportError(ErrorManager::High, 4208, "ExceptionHandler::Run", GetExceptionText(descriptive_name));
         }
         catch (...)
         {
            // Don't swallow the original exception
         }

         throw;
      }
   }



   String 
   ExceptionHandler::GetExceptionText(const String &descriptive_name)
   {
      String sErrorMessage = 
         Formatter::Format("An error occured while executing '{0}'", descriptive_name);

      return sErrorMessage;

   }

   
} 