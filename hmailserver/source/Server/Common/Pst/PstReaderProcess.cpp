// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See PstReaderProcess.h.

#include "StdAfx.h"

#include "PstReaderProcess.h"
#include "PstReaderChild.h"
#include "../Util/PstImportPath.h"
#include "../Util/Unicode.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

// A sanitised build reserves more address space than any limit a reader could
// be given, so the reader's address-space limit is left off there.
#if defined(__SANITIZE_ADDRESS__)
#define HM_PST_READER_NO_ADDRESS_LIMIT 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) || __has_feature(thread_sanitizer)
#define HM_PST_READER_NO_ADDRESS_LIMIT 1
#endif
#endif

namespace HM
{
   const int PstReaderProcess::MaximumReaders;
   const int PstReaderProcess::MaximumFailuresInARow;
   const unsigned int PstReaderProcess::HelloMilliseconds;
   const unsigned int PstReaderProcess::RequestMilliseconds;

   namespace
   {
      // How often the error log hears of a reader ending, in one phase of a job.
      const int MaximumEndsReported = 10;

      // How long a reader is given to end once its input is closed, and again
      // once it has been killed.
      const unsigned int EndMilliseconds = 5000;

#ifdef _WIN32
      // The reader's committed memory: a message of 128 MB and its rebuild, with
      // room for the program itself.
      const unsigned long long MemoryLimitBytes = 2048ULL * 1024 * 1024;
#else
      // The reader's address space: the same, with room for the shared
      // libraries' mappings and the allocator's arenas.
      const unsigned long long AddressSpaceBytes = 4096ULL * 1024 * 1024;
#endif

      unsigned long long NowMilliseconds()
      {
         return (unsigned long long) std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
      }

      String Wide(const std::string &utf8)
      {
         String text;
         Unicode::MultiByteToWide(AnsiString(utf8), text);
         return text;
      }

      std::string Nid(uint32_t nid)
      {
         return "0x" + PstFormat::Hex32(nid);
      }

      // The readers running in this service, against MaximumReaders.
      std::mutex &SlotLock()
      {
         static std::mutex lock;
         return lock;
      }

      std::condition_variable &SlotFreed()
      {
         static std::condition_variable freed;
         return freed;
      }

      int &SlotsTaken()
      {
         static int taken = 0;
         return taken;
      }

      // Set, for the life of the service, when a lowered start failed and an
      // ordinary one worked (the header).
      std::atomic<bool> &LoweringGivenUp()
      {
         static std::atomic<bool> givenUp(false);
         return givenUp;
      }

#ifdef _WIN32
      // The service's own token with every privilege removed but
      // SeChangeNotifyPrivilege; NULL with the error when it cannot be made.
      HANDLE LoweredToken(DWORD &error)
      {
         error = 0;
         HANDLE own = NULL;
         if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, &own))
         {
            error = GetLastError();
            return NULL;
         }

         HANDLE restricted = NULL;
         const BOOL made = CreateRestrictedToken(own, DISABLE_MAX_PRIVILEGE, 0, NULL, 0, NULL, 0, NULL, &restricted);
         if (!made)
            error = GetLastError();
         CloseHandle(own);
         return made ? restricted : NULL;
      }

      std::string ErrorText(const char *what, DWORD error)
      {
         return std::string(what) + " (Windows error " + std::to_string((unsigned long) error) + ")";
      }
#else
      std::string ErrorText(const char *what, int error)
      {
         return std::string(what) + " (" + std::string(std::strerror(error)) + ")";
      }
#endif
   }

   PstReaderProcess::PstReaderProcess(std::shared_ptr<PstFileSource> source, const std::function<bool()> &stop, const String &label,
                                      const std::function<void()> &restarted, const TestHook &hook) :
      source_(source),
      stop_(stop),
      label_(label),
      restarted_(restarted),
      hook_(hook)
   {
   }

   PstReaderProcess::~PstReaderProcess()
   {
      try
      {
         // Its input closed, the reader ends at the end of it; one that does not
         // is killed.
         if (HasProcess_())
         {
            std::string how;
            Discard_(how);
         }
      }
      catch (std::exception &)
      {
         // Nothing a destructor can say to anybody.
      }

      {
         std::lock_guard<std::mutex> lock(mutex_);
         closing_ = true;
      }
      wake_.notify_all();
      if (watchdog_.joinable())
         watchdog_.join();

#ifdef _WIN32
      if (thread_ != NULL)
         CloseHandle(thread_);
      thread_ = NULL;
#endif

      GiveSlot_();
   }

   // ------------------------------------------------------------ the requests

   std::string
   PstReaderProcess::OpenRequest_() const
   {
      PstOpenRequest request;
      request.password = password_;
      request.itemsUsed = items_used_;
      request.bytesUsed = bytes_used_;
      request.endAt = hook_.always;
      request.endAt.insert(request.endAt.end(), hook_.once.begin(), hook_.once.end());
      PstWireWriter writer;
      PstWire::PutOpenRequest(writer, request);
      return writer.Data();
   }

   void
   PstReaderProcess::Open(const std::string &password)
   {
      password_ = password;

      // The request is made at each attempt (Call_), from the file's cost so
      // far: a reader that ended in the opening is charged for it too.
      Call_(PstWire::KindOpen, std::string(), "opening the file's store", 0, [this](PstWireReader &reader, std::string &)
      {
         PstOpenReply reply;
         PstWire::GetOpenReply(reader, reply);
         reader.End();
         open_ = reply;
      });

      opened_ = true;
   }

   void
   PstReaderProcess::ListFolders(std::vector<PstFolderInfo> &folders, std::vector<std::string> &problems)
   {
      Call_(PstWire::KindListFolders, std::string(), "listing the file's folders", 0, [&](PstWireReader &reader, std::string &)
      {
         PstWire::GetFolders(reader, folders, problems);
         reader.End();
      });
   }

   void
   PstReaderProcess::ListMessages(uint32_t folderNid, std::vector<uint32_t> &messages)
   {
      PstWireWriter writer;
      writer.U32(folderNid);
      Call_(PstWire::KindListMessages, writer.Data(), "listing the messages of the folder " + Nid(folderNid), folderNid, [&](PstWireReader &reader, std::string &)
      {
         PstWire::GetNids(reader, messages);
         reader.End();
      });
   }

   void
   PstReaderProcess::ReadMail(uint32_t nid, PstMailRead &mail)
   {
      PstWireWriter writer;
      writer.U32(nid);
      Call_(PstWire::KindReadMail, writer.Data(), "reading the message " + Nid(nid), nid, [&](PstWireReader &reader, std::string &payload)
      {
         PstWire::GetMailHead(reader, mail);

         // The rebuilt message is the reply's last field: the payload made it,
         // rather than copying what may be a couple of hundred megabytes.
         size_t offset = 0;
         size_t length = 0;
         reader.Span(offset, length);
         reader.End();
         payload.erase(0, offset);
         payload.resize(length);
         mail.mime.swap(payload);
      });
   }

   void
   PstReaderProcess::ReadItemFolders(uint32_t &contacts, uint32_t &calendar, uint32_t &tasks)
   {
      PstItemFoldersReply reply;
      Call_(PstWire::KindItemFolders, std::string(), "reading the file's contacts, calendar and tasks folders", 0, [&](PstWireReader &reader, std::string &)
      {
         PstWire::GetItemFolders(reader, reply);
         reader.End();
      });

      contacts = reply.contacts;
      calendar = reply.calendar;
      tasks = reply.tasks;
      names_problem_ = reply.namesProblem;
      item_folders_read_ = true;
   }

   void
   PstReaderProcess::ReadContact(uint32_t nid, PstContactRead &contact)
   {
      PstWireWriter writer;
      writer.U32(nid);
      Call_(PstWire::KindReadContact, writer.Data(), "reading the contact " + Nid(nid), nid, [&](PstWireReader &reader, std::string &)
      {
         PstWire::GetContact(reader, contact);
         reader.End();
      });
   }

   void
   PstReaderProcess::ReadCalendarItem(uint32_t nid, bool taskFolder, PstCalendarRead &item)
   {
      PstWireWriter writer;
      writer.U32(nid);
      writer.U8(taskFolder ? 1 : 0);
      Call_(PstWire::KindReadCalendarItem, writer.Data(), std::string(taskFolder ? "reading the task " : "reading the calendar item ") + Nid(nid), nid,
         [&](PstWireReader &reader, std::string &)
      {
         PstWire::GetCalendarItem(reader, item);
         reader.End();
      });
   }

   void
   PstReaderProcess::Rethrow(const PstReplyStatus &status)
   {
      switch (status.status)
      {
      case PstWire::StatusDone:
         return;
      case PstWire::StatusRefused:
         throw PstRefusal((PstRefusal::Reason) status.refusalReason, status.sentence);
      case PstWire::StatusDamaged:
         throw PstError(status.sentence);
      case PstWire::StatusTooLarge:
         throw PstTooLarge(status.sentence);
      case PstWire::StatusBudget:
         throw PstBudgetExhausted(status.sentence, status.wholeFile);
      case PstWire::StatusMemory:
         throw std::bad_alloc();
      default:
         throw std::runtime_error(status.sentence);
      }
   }

   void
   PstReaderProcess::Call_(unsigned kind, const std::string &request, const std::string &what, uint32_t nid, const BodyReader &body)
   {
      // What the reader in the service did at its first read.
      if (stop_ && stop_())
         throw PstStopped();

      for (int attempt = 1; ; attempt++)
      {
         std::string why;
         std::string payload;
         Exchange result = ExchangeEnded;

         if (running_ || Start_(why))
            result = Exchange_(kind, kind == PstWire::KindOpen ? OpenRequest_() : request, RequestMilliseconds, payload, why);

         if (result == ExchangeStopped)
         {
            std::string how;
            Discard_(how);
            throw PstStopped();
         }

         if (result == ExchangeAnswered)
         {
            PstReplyStatus status;
            try
            {
               PstWireReader reader(payload);
               PstWire::GetStatus(reader, status);

               file_exhausted_ = status.fileExhausted;
               items_used_ = status.itemsUsed;
               bytes_used_ = status.bytesUsed;
               failures_in_a_row_ = 0;

               if (status.status == PstWire::StatusDone)
                  body(reader, payload);
            }
            catch (PstWireError &error)
            {
               // A reply that is not one: the reader is not to be believed, and
               // is ended as if it had ended itself.
               why = std::string("it answered with a reply that is not one: ") + error.what();
               result = ExchangeEnded;
            }

            if (result == ExchangeAnswered)
            {
               Rethrow(status);
               return;
            }
         }

         // Ended: said, and the process gone.
         std::string how;
         Discard_(how);
         Report_(what, how.empty() ? why : why + ", " + how);

         // What the reader did on the request it ended in was never answered,
         // so it is charged as the most one request may cost - a whole unit of
         // the work budget - and the next reader starts from there: no file
         // earns work past its budget by ending the reader, or by making it run
         // until it is killed.
         items_used_ += PstBudget::UnitItems;
         bytes_used_ += PstBudget::UnitBytes;

         // The test hook's once: the reader ends at such a NID the first time
         // it is asked for it, and not again.
         for (std::vector<uint32_t>::iterator once = hook_.once.begin(); once != hook_.once.end(); )
            once = *once == nid ? hook_.once.erase(once) : once + 1;

         if (attempt >= 2)
         {
            failures_in_a_row_++;
            throw PstReaderFailed("the PST reader's process ended twice while " + what + " (" + (how.empty() ? why : why + ", " + how) + ")");
         }

         if (stop_ && stop_())
            throw PstStopped();
      }
   }

   PstReaderProcess::Exchange
   PstReaderProcess::Exchange_(unsigned kind, const std::string &request, unsigned int milliseconds, std::string &payload, std::string &why)
   {
      Arm_(milliseconds);

      bool sent = true;
      if (kind != 0)
         sent = Write_(PstWire::Header(kind, request.size()) + request);

      unsigned replyKind = 0;
      std::string problem;
      PstWire::FrameResult frame = PstWire::FrameBroken;
      if (sent)
         frame = PstWire::ReadFrame([this](char *buffer, size_t length) { return Read_(buffer, length); }, replyKind, payload, problem);

      const int killedFor = Disarm_();
      if (killedFor == KilledForStop)
         return ExchangeStopped;
      if (killedFor == KilledForTime)
      {
         why = "it had not answered in " + std::to_string(milliseconds / 1000) + " seconds and was ended";
         return ExchangeEnded;
      }

      if (!sent)
      {
         why = "it no longer read what it was sent";
         return ExchangeEnded;
      }
      if (frame == PstWire::FrameEnded)
      {
         why = "its output ended";
         return ExchangeEnded;
      }
      if (frame != PstWire::FrameRead)
      {
         why = problem;
         return ExchangeEnded;
      }

      const unsigned expected = kind == 0 ? (unsigned) PstWire::KindHello : (kind | PstWire::KindReply);
      if (replyKind != expected)
      {
         why = "it answered a request it was not sent";
         return ExchangeEnded;
      }

      return ExchangeAnswered;
   }

   // ------------------------------------------------------------ the process

   bool
   PstReaderProcess::Start_(std::string &why)
   {
      TakeSlot_();

      if (!watchdog_.joinable())
         watchdog_ = std::thread([this]() { Watch_(); });

      bool lowered = !LoweringGivenUp().load();
      for (;;)
      {
         bool loweringRefused = false;
         std::string startWhy;
         bool spawned = Spawn_(lowered, loweringRefused, startWhy);

         std::string how;
         if (spawned)
         {
            // Its hello: the protocol's version, and the file taken.
            std::string payload;
            const Exchange hello = Exchange_(0, std::string(), HelloMilliseconds, payload, startWhy);
            if (hello == ExchangeStopped)
            {
               Discard_(how);
               throw PstStopped();
            }

            if (hello == ExchangeAnswered)
            {
               try
               {
                  PstWireReader reader(payload);
                  uint32_t version = 0;
                  uint32_t processId = 0;
                  bool fileTaken = false;
                  PstWire::GetHello(reader, version, processId, fileTaken);
                  reader.End();

                  if (version != PstWire::ProtocolVersion)
                     startWhy = "it is another version of this program (protocol " + std::to_string(version) + ", not " +
                                std::to_string(PstWire::ProtocolVersion) + ") - the program file was replaced while the service ran; restart the service";
                  else if (!fileTaken)
                     startWhy = "it could not take the file it was given";
                  else
                     break;
               }
               catch (PstWireError &error)
               {
                  startWhy = error.what();
               }
            }

            Discard_(how);
            if (!how.empty())
               startWhy += ", " + how;

            loweringRefused = lowered;
         }

         // The lowered start failed: once more without the lowering.
         if (lowered && loweringRefused)
         {
            lowered = false;
            continue;
         }

         if (!unavailable_reported_)
         {
            unavailable_reported_ = true;
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7550, "PstReaderProcess::Start_",
               Formatter::Format(_T("{0}: the PST reader's process could not be started: {1}. The job pauses and tries again."), label_, Wide(startWhy)));
         }

         throw PstReaderUnavailable("The PST reader's process could not be started: " + startWhy + ". The job tries again later.");
      }

      // A lowered start that failed and an ordinary one that worked: the
      // lowering is given up for the life of the service, and said once.
      if (!lowered && !LoweringGivenUp().exchange(true))
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7552, "PstReaderProcess::Start_",
            _T("The PST reader's process could not be started with lowered rights, and was started without them: on Windows with the service's own token, in a job object where the system allows one, on Linux in a process group of its own without its resource limits. It is still a process of its own that the service ends on a stop. Until the service restarts every PST reader starts this way."));
      }

      running_ = true;
      if (started_before_ && restarted_)
         restarted_();
      started_before_ = true;

      // The store opened again, as it was: a new reader goes on from where the
      // last one left the file's budget.
      return Replay_(why);
   }

   bool
   PstReaderProcess::Replay_(std::string &why)
   {
      if (!opened_)
         return true;

      std::vector<std::pair<unsigned, std::string> > replays;
      replays.push_back(std::make_pair((unsigned) PstWire::KindOpen, OpenRequest_()));
      if (item_folders_read_)
         replays.push_back(std::make_pair((unsigned) PstWire::KindItemFolders, std::string()));

      for (const std::pair<unsigned, std::string> &replay : replays)
      {
         std::string payload;
         const Exchange result = Exchange_(replay.first, replay.second, RequestMilliseconds, payload, why);
         if (result == ExchangeStopped)
         {
            std::string how;
            Discard_(how);
            throw PstStopped();
         }
         if (result != ExchangeAnswered)
            return false;

         PstReplyStatus status;
         try
         {
            PstWireReader reader(payload);
            PstWire::GetStatus(reader, status);
         }
         catch (PstWireError &error)
         {
            why = error.what();
            return false;
         }

         file_exhausted_ = status.fileExhausted;
         items_used_ = status.itemsUsed;
         bytes_used_ = status.bytesUsed;

         // The store refused, not the reader ended - the whole file's budget
         // spent by the reopening above all: thrown as what it is, as the
         // first opening's refusal is. The reader, whose store is not open,
         // goes, and the next request starts another.
         if (status.status != PstWire::StatusDone)
         {
            std::string how;
            Discard_(how);
            Rethrow(status);
         }
      }

      return true;
   }

   void
   PstReaderProcess::Report_(const std::string &what, const std::string &why)
   {
      if (ends_reported_ >= MaximumEndsReported)
         return;
      ends_reported_++;

      ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7551, "PstReaderProcess::Call_",
         Formatter::Format(_T("{0}: the PST reader's process ended while {1} ({2}). The service goes on with a new one; a message or an item it ends on twice is left out and counted reader_failed in the job's report."),
            label_, Wide(what), Wide(why)));
   }

   void
   PstReaderProcess::TakeSlot_()
   {
      if (has_slot_)
         return;

      std::unique_lock<std::mutex> lock(SlotLock());
      while (SlotsTaken() >= MaximumReaders)
      {
         if (stop_ && stop_())
            throw PstStopped();
         SlotFreed().wait_for(lock, std::chrono::milliseconds(250));
      }
      SlotsTaken()++;
      has_slot_ = true;
   }

   void
   PstReaderProcess::GiveSlot_()
   {
      if (!has_slot_)
         return;

      {
         std::lock_guard<std::mutex> lock(SlotLock());
         SlotsTaken()--;
      }
      has_slot_ = false;
      SlotFreed().notify_all();
   }

   void
   PstReaderProcess::Arm_(unsigned int milliseconds)
   {
      std::lock_guard<std::mutex> lock(mutex_);
      in_request_ = true;
      deadline_ = NowMilliseconds() + milliseconds;

#ifdef _WIN32
      // The job's thread, the one that waits on the reader's output, for the
      // watchdog to cancel that wait (Watch_).
      if (thread_ == NULL && !DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread_, THREAD_TERMINATE, FALSE, 0))
         thread_ = NULL;
#endif
   }

   int
   PstReaderProcess::Disarm_()
   {
      std::lock_guard<std::mutex> lock(mutex_);
      in_request_ = false;
      return killed_for_;
   }

   void
   PstReaderProcess::Watch_()
   {
      std::unique_lock<std::mutex> lock(mutex_);
      while (!closing_)
      {
         wake_.wait_for(lock, std::chrono::milliseconds(100));
         if (closing_)
            break;

         if (killed_for_ == KilledForNothing && HasProcess_())
         {
            // A stop ends the reader whatever it is doing; a request past its
            // time ends it too.
            const bool stop = stop_ && stop_();
            const bool late = in_request_ && NowMilliseconds() > deadline_;
            if (stop || late)
            {
               killed_for_ = stop ? KilledForStop : KilledForTime;
               Kill_();
            }
         }

#ifdef _WIN32
         // The reader killed, its output ends and the job's thread has its
         // answer - unless some other process inherited the pipe's far end in
         // the moment it was inheritable, and holds it open. So the wait is
         // cancelled as well, at every tick until the job's thread is out of it.
         if (killed_for_ != KilledForNothing && in_request_ && thread_ != NULL)
            CancelSynchronousIo(thread_);
#endif
      }
   }

#ifdef _WIN32

   bool
   PstReaderProcess::HasProcess_() const
   {
      return process_ != NULL;
   }

   void
   PstReaderProcess::Kill_()
   {
      if (job_ != NULL)
         TerminateJobObject(job_, 1);
      else if (process_ != NULL)
         TerminateProcess(process_, 1);
   }

   bool
   PstReaderProcess::Write_(const std::string &data)
   {
      const char *at = data.data();
      size_t left = data.size();
      while (left > 0)
      {
         const DWORD chunk = left > (size_t) (1 << 20) ? (DWORD) (1 << 20) : (DWORD) left;
         DWORD written = 0;
         if (!WriteFile(input_, at, chunk, &written, NULL) || written == 0)
            return false;
         at += written;
         left -= (size_t) written;
      }
      return true;
   }

   long long
   PstReaderProcess::Read_(char *buffer, size_t length)
   {
      const DWORD want = length > (size_t) (1 << 20) ? (DWORD) (1 << 20) : (DWORD) length;
      DWORD got = 0;
      if (!ReadFile(output_, buffer, want, &got, NULL))
         return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
      return (long long) got;
   }

   bool
   PstReaderProcess::Spawn_(bool lowered, bool &loweringRefused, std::string &why)
   {
      loweringRefused = false;

      std::vector<wchar_t> module(32768);
      const DWORD length = GetModuleFileNameW(NULL, &module[0], (DWORD) module.size());
      if (length == 0 || length >= (DWORD) module.size())
      {
         why = ErrorText("the server's own program file could not be named", GetLastError());
         return false;
      }
      const std::wstring program(&module[0], (size_t) length);
      std::wstring directory = program;
      const size_t slash = directory.find_last_of(L"\\/");
      if (slash != std::wstring::npos)
         directory.resize(slash);

      // Every handle the reader is given is inheritable, and only those four
      // are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): the two pipes' far
      // ends, the null device for its errors, and a copy of the file's handle.
      SECURITY_ATTRIBUTES inheritable;
      ZeroMemory(&inheritable, sizeof(inheritable));
      inheritable.nLength = sizeof(inheritable);
      inheritable.bInheritHandle = TRUE;

      HANDLE childInput = NULL;
      HANDLE parentInput = NULL;
      HANDLE parentOutput = NULL;
      HANDLE childOutput = NULL;
      HANDLE childError = INVALID_HANDLE_VALUE;
      HANDLE childFile = NULL;

      auto closeChildEnds = [&]()
      {
         if (childInput) CloseHandle(childInput);
         if (childOutput) CloseHandle(childOutput);
         if (childError != INVALID_HANDLE_VALUE) CloseHandle(childError);
         if (childFile) CloseHandle(childFile);
         childInput = childOutput = childFile = NULL;
         childError = INVALID_HANDLE_VALUE;
      };
      auto closeParentEnds = [&]()
      {
         if (parentInput) CloseHandle(parentInput);
         if (parentOutput) CloseHandle(parentOutput);
         parentInput = parentOutput = NULL;
      };

      if (!CreatePipe(&childInput, &parentInput, &inheritable, 0) || !CreatePipe(&parentOutput, &childOutput, &inheritable, 1024 * 1024))
      {
         why = ErrorText("its pipes could not be made", GetLastError());
         closeChildEnds();
         closeParentEnds();
         return false;
      }
      SetHandleInformation(parentInput, HANDLE_FLAG_INHERIT, 0);
      SetHandleInformation(parentOutput, HANDLE_FLAG_INHERIT, 0);

      childError = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, NULL);
      if (childError == INVALID_HANDLE_VALUE ||
          !DuplicateHandle(GetCurrentProcess(), (HANDLE) source_->Native(), GetCurrentProcess(), &childFile, GENERIC_READ, TRUE, 0))
      {
         why = ErrorText("the file's handle could not be given to it", GetLastError());
         childFile = NULL;
         closeChildEnds();
         closeParentEnds();
         return false;
      }

      SIZE_T attributeSize = 0;
      InitializeProcThreadAttributeList(NULL, 1, 0, &attributeSize);
      std::vector<unsigned char> attributeBuffer(attributeSize > 0 ? attributeSize : 1);
      LPPROC_THREAD_ATTRIBUTE_LIST attributes = (LPPROC_THREAD_ATTRIBUTE_LIST) &attributeBuffer[0];
      HANDLE inherited[4] = { childInput, childOutput, childError, childFile };
      if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize))
      {
         why = ErrorText("the list of handles it inherits could not be made", GetLastError());
         closeChildEnds();
         closeParentEnds();
         return false;
      }
      if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), NULL, NULL))
      {
         why = ErrorText("the list of handles it inherits could not be made", GetLastError());
         DeleteProcThreadAttributeList(attributes);
         closeChildEnds();
         closeParentEnds();
         return false;
      }

      STARTUPINFOEXW startup;
      ZeroMemory(&startup, sizeof(startup));
      startup.StartupInfo.cb = sizeof(startup);
      startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
      startup.StartupInfo.wShowWindow = SW_HIDE;
      startup.StartupInfo.hStdInput = childInput;
      startup.StartupInfo.hStdOutput = childOutput;
      startup.StartupInfo.hStdError = childError;
      startup.lpAttributeList = attributes;

      const std::wstring commandLine = L"\"" + program + L"\" --pst-read " + std::to_wstring((unsigned long long) (uintptr_t) childFile);
      std::vector<wchar_t> line(commandLine.begin(), commandLine.end());
      line.push_back(L'\0');

      // Suspended until it is in its job object, so that it runs nothing
      // outside it.
      const DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;

      HANDLE token = NULL;
      DWORD tokenError = 0;
      if (lowered)
      {
         token = LoweredToken(tokenError);
         if (token == NULL)
         {
            why = ErrorText("its restricted token could not be made", tokenError);
            loweringRefused = true;
            DeleteProcThreadAttributeList(attributes);
            closeChildEnds();
            closeParentEnds();
            return false;
         }
      }

      PROCESS_INFORMATION information;
      ZeroMemory(&information, sizeof(information));
      const BOOL started = token != NULL
         ? CreateProcessAsUserW(token, program.c_str(), &line[0], NULL, NULL, TRUE, flags, NULL, directory.c_str(), &startup.StartupInfo, &information)
         : CreateProcessW(program.c_str(), &line[0], NULL, NULL, TRUE, flags, NULL, directory.c_str(), &startup.StartupInfo, &information);
      const DWORD startError = GetLastError();

      if (token != NULL)
         CloseHandle(token);
      DeleteProcThreadAttributeList(attributes);

      // The reader has its copies, or there is no reader.
      closeChildEnds();

      if (!started)
      {
         why = ErrorText(lowered ? "it could not be started with its restricted token" : "it could not be started", startError);
         loweringRefused = lowered;
         closeParentEnds();
         return false;
      }

      // The job object: what the reader may do, and its end with the job's
      // handle, which is this object's and, should the service end, the
      // system's to close.
      HANDLE job = CreateJobObjectW(NULL, NULL);
      bool jobbed = job != NULL;
      DWORD jobError = jobbed ? 0 : GetLastError();

      if (jobbed)
      {
         JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
         ZeroMemory(&limits, sizeof(limits));
         limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
                                                   JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
         limits.BasicLimitInformation.ActiveProcessLimit = 1;
         limits.ProcessMemoryLimit = (SIZE_T) MemoryLimitBytes;
         if (lowered)
         {
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PRIORITY_CLASS;
            limits.BasicLimitInformation.PriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
         }
         jobbed = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != FALSE;

         if (jobbed && lowered)
         {
            JOBOBJECT_BASIC_UI_RESTRICTIONS interfaceLimits;
            ZeroMemory(&interfaceLimits, sizeof(interfaceLimits));
            interfaceLimits.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_EXITWINDOWS |
                                                  JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                                                  JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
            jobbed = SetInformationJobObject(job, JobObjectBasicUIRestrictions, &interfaceLimits, sizeof(interfaceLimits)) != FALSE;
         }

         jobbed = jobbed && AssignProcessToJobObject(job, information.hProcess) != FALSE;
         if (!jobbed)
            jobError = GetLastError();
      }

      if (!jobbed && lowered)
      {
         TerminateProcess(information.hProcess, 1);
         WaitForSingleObject(information.hProcess, EndMilliseconds);
         CloseHandle(information.hThread);
         CloseHandle(information.hProcess);
         if (job != NULL)
            CloseHandle(job);
         closeParentEnds();
         why = ErrorText("its job object could not be made", jobError);
         loweringRefused = true;
         return false;
      }

      // The start without the lowering, where even a job object of its own is
      // refused (a service already in a job that allows none inside it): the
      // reader runs, killed by its own handle and ending with its input when
      // the service ends.
      if (!jobbed && job != NULL)
      {
         CloseHandle(job);
         job = NULL;
      }

      {
         std::lock_guard<std::mutex> lock(mutex_);
         process_ = information.hProcess;
         job_ = job;
         killed_for_ = KilledForNothing;
      }
      input_ = parentInput;
      output_ = parentOutput;

      ResumeThread(information.hThread);
      CloseHandle(information.hThread);
      return true;
   }

   void
   PstReaderProcess::Discard_(std::string &how)
   {
      running_ = false;
      how.clear();

      // Its pipes first: a reader waiting for a request ends at the end of its
      // input, and one writing a reply nobody reads has its write fail.
      if (input_ != NULL)
         CloseHandle(input_);
      if (output_ != NULL)
         CloseHandle(output_);
      input_ = NULL;
      output_ = NULL;

      HANDLE process = NULL;
      {
         std::lock_guard<std::mutex> lock(mutex_);
         process = process_;
      }
      if (process == NULL)
         return;

      if (WaitForSingleObject(process, EndMilliseconds) != WAIT_OBJECT_0)
      {
         {
            std::lock_guard<std::mutex> lock(mutex_);
            Kill_();
         }
         WaitForSingleObject(process, EndMilliseconds);
      }

      DWORD code = 0;
      if (GetExitCodeProcess(process, &code) && code != STILL_ACTIVE)
      {
         char text[64];
         if (code >= 0xC0000000)
            snprintf(text, sizeof(text), "exit code 0x%08lX", (unsigned long) code);
         else
            snprintf(text, sizeof(text), "exit code %lu", (unsigned long) code);
         how = text;
      }
      else
         how = "it did not end";

      std::lock_guard<std::mutex> lock(mutex_);
      CloseHandle(process_);
      process_ = NULL;
      if (job_ != NULL)
         CloseHandle(job_);
      job_ = NULL;
   }

#else

   bool
   PstReaderProcess::HasProcess_() const
   {
      return pid_ > 0;
   }

   void
   PstReaderProcess::Kill_()
   {
      // The group first: whatever the reader started, if it could start
      // anything, goes with it.
      if (pid_ > 0)
      {
         ::kill(-pid_, SIGKILL);
         ::kill(pid_, SIGKILL);
      }
   }

   bool
   PstReaderProcess::Write_(const std::string &data)
   {
      const char *at = data.data();
      size_t left = data.size();
      while (left > 0)
      {
         const ssize_t written = ::write(input_, at, left);
         if (written < 0 && errno == EINTR)
            continue;
         if (written <= 0)
            return false;
         at += written;
         left -= (size_t) written;
      }
      return true;
   }

   long long
   PstReaderProcess::Read_(char *buffer, size_t length)
   {
      // In slices of a tenth of a second, so that a reader the watchdog has
      // killed ends the wait even where its output does not end - some other
      // process holding the pipe's far end, inherited in the moment before its
      // exec closed it.
      for (;;)
      {
         struct pollfd ready;
         ready.fd = output_;
         ready.events = POLLIN;
         ready.revents = 0;
         const int polled = ::poll(&ready, 1, 100);
         if (polled < 0)
         {
            if (errno == EINTR)
               continue;
            return -1;
         }
         if (polled == 0)
         {
            std::lock_guard<std::mutex> lock(mutex_);
            if (killed_for_ != KilledForNothing)
               return -1;
            continue;
         }

         const ssize_t got = ::read(output_, buffer, length);
         if (got < 0 && errno == EINTR)
            continue;
         return (long long) got;
      }
   }

   bool
   PstReaderProcess::Spawn_(bool lowered, bool &loweringRefused, std::string &why)
   {
      loweringRefused = false;

      // This program, by the link the kernel keeps: the very file this
      // process runs, even if a package upgrade has replaced the one on disk.
      char self[4096];
      const ssize_t named = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
      if (named <= 0)
      {
         why = ErrorText("the server's own program file could not be named", errno);
         return false;
      }
      self[named] = '\0';

      int input[2] = { -1, -1 };
      int output[2] = { -1, -1 };
      int failure[2] = { -1, -1 };
      int file = -1;
      int nothing = -1;

      auto closeOne = [](int &descriptor)
      {
         if (descriptor >= 0)
            ::close(descriptor);
         descriptor = -1;
      };
      auto closeAll = [&]()
      {
         closeOne(input[0]);
         closeOne(input[1]);
         closeOne(output[0]);
         closeOne(output[1]);
         closeOne(failure[0]);
         closeOne(failure[1]);
         closeOne(file);
         closeOne(nothing);
      };

      // Each descriptor the reader is to keep numbered 10 or above, so that
      // none of the child's dup2 calls onto 0, 1 and 3 closes another before it
      // is copied.
      auto high = [](int &descriptor) -> bool
      {
         if (descriptor >= 10)
            return true;
         const int moved = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 10);
         if (moved < 0)
            return false;
         ::close(descriptor);
         descriptor = moved;
         return true;
      };

      if (::pipe2(input, O_CLOEXEC) != 0 || ::pipe2(output, O_CLOEXEC) != 0 || ::pipe2(failure, O_CLOEXEC) != 0)
      {
         why = ErrorText("its pipes could not be made", errno);
         closeAll();
         return false;
      }

      file = ::fcntl((int) source_->Native(), F_DUPFD_CLOEXEC, 10);
      if (file < 0 || !high(input[0]) || !high(output[1]) || !high(failure[1]))
      {
         why = ErrorText("the file's descriptor could not be given to it", errno);
         closeAll();
         return false;
      }

      // Its standard error is the null device, not the service's journal: a
      // reader that has parsed a hostile file writes nothing anybody reads.
      nothing = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
      if (nothing < 0 || !high(nothing))
      {
         why = ErrorText("the null device could not be opened for its errors", errno);
         closeAll();
         return false;
      }

      // Everything the child needs, made before the fork: between the fork and
      // the exec it calls nothing that allocates or takes a lock.
      char verbText[] = "--pst-read";
      char descriptorText[] = "3";
      char *arguments[] = { self, verbText, descriptorText, nullptr };

      struct sigaction defaults = {};
      defaults.sa_handler = SIG_DFL;
      sigemptyset(&defaults.sa_mask);
      sigset_t noSignals;
      sigemptyset(&noSignals);

      struct rlimit none;
      none.rlim_cur = 0;
      none.rlim_max = 0;
      struct rlimit descriptors;
      descriptors.rlim_cur = 64;
      descriptors.rlim_max = 64;
#ifndef HM_PST_READER_NO_ADDRESS_LIMIT
      struct rlimit memory;
      memory.rlim_cur = (rlim_t) AddressSpaceBytes;
      memory.rlim_max = (rlim_t) AddressSpaceBytes;
#endif

      // How far the close below has to go where the kernel has no close_range.
      struct rlimit current;
      int highest = 65536;
      if (::getrlimit(RLIMIT_NOFILE, &current) == 0)
         highest = current.rlim_cur == RLIM_INFINITY || current.rlim_cur > (rlim_t) (1 << 20) ? (1 << 20) : (int) current.rlim_cur;

      const pid_t parent = ::getpid();
      const int requests = input[0];
      const int replies = output[1];
      const int report = failure[1];
      const int pst = file;
      const int silence = nothing;

      const pid_t pid = ::fork();
      if (pid < 0)
      {
         why = ErrorText("it could not be started", errno);
         closeAll();
         return false;
      }

      if (pid == 0)
      {
         // The reader, before it is the reader.
         ::setpgid(0, 0);
         ::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
         if (::getppid() != parent)
            ::_exit(126);

         // The service ignores SIGPIPE, and an ignored signal stays ignored
         // across exec: the reader writing to a service that is gone should end.
         ::sigaction(SIGPIPE, &defaults, nullptr);
         ::sigprocmask(SIG_SETMASK, &noSignals, nullptr);

         if (::dup2(requests, 0) < 0 || ::dup2(replies, 1) < 0 || ::dup2(silence, 2) < 0 || ::dup2(pst, 3) < 0)
         {
            const int error = errno;
            const ssize_t ignored = ::write(report, &error, sizeof(error));
            (void) ignored;
            ::_exit(127);
         }

         // Nothing else of the service's: every descriptor from 4 up is closed
         // but the failure pipe, which the exec closes.
         bool closed = false;
#ifdef SYS_close_range
         closed = (report <= 4 || ::syscall(SYS_close_range, 4U, (unsigned int) (report - 1), 0U) == 0) &&
                  ::syscall(SYS_close_range, (unsigned int) (report + 1), ~0U, 0U) == 0;
#endif
         if (!closed)
         {
            for (int descriptorNumber = 4; descriptorNumber < highest; descriptorNumber++)
            {
               if (descriptorNumber != report)
                  ::close(descriptorNumber);
            }
         }

         if (lowered)
         {
            ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
#ifdef PR_CAP_AMBIENT
            ::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);
#endif
            ::setrlimit(RLIMIT_CORE, &none);
            ::setrlimit(RLIMIT_FSIZE, &none);
            ::setrlimit(RLIMIT_NOFILE, &descriptors);
#ifndef HM_PST_READER_NO_ADDRESS_LIMIT
            ::setrlimit(RLIMIT_AS, &memory);
#endif
            ::setpriority(PRIO_PROCESS, 0, 10);
         }

         ::execv("/proc/self/exe", arguments);

         const int error = errno;
         const ssize_t ignored = ::write(report, &error, sizeof(error));
         (void) ignored;
         ::_exit(127);
      }

      // Either this or the child's own call makes the group; both say the same.
      ::setpgid(pid, pid);

      closeOne(input[0]);
      closeOne(output[1]);
      closeOne(failure[1]);
      closeOne(file);
      closeOne(nothing);

      // The failure pipe closes on a successful exec; four bytes are the
      // reason there was none.
      int childError = 0;
      ssize_t got = 0;
      for (;;)
      {
         got = ::read(failure[0], &childError, sizeof(childError));
         if (got >= 0 || errno != EINTR)
            break;
      }
      closeOne(failure[0]);

      if (got == (ssize_t) sizeof(childError))
      {
         int status = 0;
         while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
         {
         }
         closeAll();
         why = ErrorText("it could not be started", childError);
         loweringRefused = lowered;
         return false;
      }

      {
         std::lock_guard<std::mutex> lock(mutex_);
         pid_ = (int) pid;
         killed_for_ = KilledForNothing;
      }
      input_ = input[1];
      output_ = output[0];
      return true;
   }

   void
   PstReaderProcess::Discard_(std::string &how)
   {
      running_ = false;
      how.clear();

      if (input_ >= 0)
         ::close(input_);
      if (output_ >= 0)
         ::close(output_);
      input_ = -1;
      output_ = -1;

      // Waited for without holding the lock across the wait, so that the
      // watchdog can still kill it; reaped, and forgotten, under the lock, so
      // that the watchdog never signals a number the system has given again.
      auto reap = [&](unsigned int milliseconds, int &status) -> bool
      {
         const unsigned long long until = NowMilliseconds() + milliseconds;
         for (;;)
         {
            {
               std::lock_guard<std::mutex> lock(mutex_);
               if (pid_ <= 0)
                  return false;
               const pid_t reaped = ::waitpid((pid_t) pid_, &status, WNOHANG);
               if (reaped == (pid_t) pid_ || (reaped < 0 && errno == ECHILD))
               {
                  pid_ = -1;
                  return reaped > 0;
               }
            }
            if (NowMilliseconds() >= until)
               return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
         }
      };

      {
         std::lock_guard<std::mutex> lock(mutex_);
         if (pid_ <= 0)
            return;
      }

      int status = 0;
      bool reaped = reap(EndMilliseconds, status);
      if (!reaped)
      {
         {
            std::lock_guard<std::mutex> lock(mutex_);
            Kill_();
         }
         reaped = reap(EndMilliseconds, status);
      }

      if (reaped && WIFEXITED(status))
         how = "exit code " + std::to_string(WEXITSTATUS(status));
      else if (reaped && WIFSIGNALED(status))
         how = "ended by signal " + std::to_string(WTERMSIG(status));
      else
         how = "it did not end";

      // One that would not end even when killed - in an uninterruptible read
      // of a dead mount - is left to its death signal, and forgotten here.
      std::lock_guard<std::mutex> lock(mutex_);
      pid_ = -1;
   }

#endif

   // ------------------------------------------------------------ the test hook

   bool
   PstReaderProcess::ReadTestHook(const String &file, const String &directory, TestHook &hook, String &said)
   {
      hook = TestHook();
      said.Empty();

      const String path = file + _T(".reader-test");
      String finalPath;
      bool isDirectory = false;
      if (!FileUtilities::Exists(path) || !PstImportPath::FinalPathOf(path, finalPath, isDirectory) || isDirectory ||
          !PstImportPath::IsInside(finalPath, directory))
         return false;

      const long size = FileUtilities::FileSize(path);
      if (size <= 0 || size > 4096)
         return false;

      const AnsiString text(FileUtilities::ReadCompleteTextFile(path));
      const std::string content(text.c_str());

      std::string always;
      std::string once;
      size_t from = 0;
      while (from < content.size())
      {
         size_t end = content.find('\n', from);
         if (end == std::string::npos)
            end = content.size();
         std::string line = content.substr(from, end - from);
         from = end + 1;

         while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();

         const bool isOnce = line.compare(0, 12, "end-once-at ") == 0;
         const bool isAlways = line.compare(0, 7, "end-at ") == 0;
         if (!isOnce && !isAlways)
            continue;

         const std::string number = line.substr(isOnce ? 12 : 7);
         char *stop = nullptr;
         const unsigned long nid = std::strtoul(number.c_str(), &stop, 16);
         if (number.empty() || stop == nullptr || *stop != '\0' || nid == 0 || nid > 0xFFFFFFFFUL)
            continue;

         (isOnce ? hook.once : hook.always).push_back((uint32_t) nid);
         std::string &list = isOnce ? once : always;
         list += (list.empty() ? "" : ", ") + Nid((uint32_t) nid);
      }

      if (hook.always.empty() && hook.once.empty())
         return false;

      said = Formatter::Format(_T("the PST reader's test hook, {0}, is in effect: the reader's process ends itself, as a crash would, when it is asked for {1}{2}{3}. Remove the file to import those messages."),
         path, Wide(always.empty() ? std::string() : always + " (every time)"), String(!always.empty() && !once.empty() ? _T(" and ") : _T("")),
         Wide(once.empty() ? std::string() : once + " (the first time)"));
      return true;
   }
}
