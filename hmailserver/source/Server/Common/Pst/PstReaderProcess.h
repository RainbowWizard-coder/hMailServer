// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The PST reader, run outside the service (docs/Migration.md, "Outlook data
// files (PST)"): the service's half. A PST job's phase makes one of these for its
// file, and asks it what it asked PstStore when the reader ran in the service -
// open the store, list the folders, list a folder's messages, read a message -
// and each question goes to a process of its own, this server's own program
// started as the reader (PstReaderChild.h), a frame each way (PstReaderWire.h).
//
// THE PROCESS. It is given the handle (Windows) or descriptor (Linux) the job
// opened and checked, inherited and named on its command line, and nothing
// else of the service's: its standard input carries the requests, its standard
// output the replies, and its standard error is the null device. It is CRASH
// CONTAINMENT, NOT A SANDBOX: it runs as the service's own account - on Windows
// LocalSystem, or the service's virtual account, at the service's integrity
// level; on Linux the service's user - and what bounds it is this:
//
//   Windows  a restricted token - every privilege removed but the one that
//            lets a process walk a path (DISABLE_MAX_PRIVILEGE) - in a job
//            object of its own: one process and no more (it can start
//            nothing), 2 GB of memory, below-normal priority, the desktop,
//            the clipboard, global atoms and other processes' windows closed to
//            it, killed when the job's handle closes (so with the service, if
//            the service ends) and on an unhandled exception without a dialog.
//            Not a lower integrity level: this program is a Windows one, and at
//            low integrity its user32 cannot attach to the service's desktop.
//   Linux    its own process group, killed with the thread that started it
//            (PR_SET_PDEATHSIG), no new privileges and no ambient capability
//            (the service's CAP_NET_BIND_SERVICE is not passed on), 4 GB of
//            address space, no core file, no file it may write, 64 descriptors,
//            nice 10, and every descriptor of the service's closed but the
//            three it is given.
//
// Should the lowered start fail - the token refused, or the process ending
// before it says hello - one start without the lowering is tried (a job object
// where the system allows one, or the process group and the death signal,
// stay), and if that one
// works the lowering is given up for the life of the service and HM7552 says so
// once. No start at all is HM7550, and the job pauses and tries again later
// (PstReaderUnavailable).
//
// A STOP, A PAUSE, A CANCEL. A watchdog thread asks the job's stop every tenth of
// a second while the reader lives, and on a stop kills it - the job object, or
// the process group - so the job's thread, waiting for a reply, has its answer
// at once: PstStopped, as the reader in the service threw between two reads. The
// job then writes its marks as it always has. The same watchdog kills a reader
// that has not answered one request in RequestMilliseconds (ten minutes: the
// work budget bounds a message at 4,000,000 reads, which no disk takes ten
// minutes over), and one that has not said hello in HelloMilliseconds.
//
// A READER THAT ENDS. Its output ending in the middle of a request - a crash, a
// kill for time, a frame that is not one, a reply to another request - is the
// reader failing on that request. The service reports it (HM7551, at most ten a
// phase), starts a new reader, opens the store in it again with the work the
// file has cost so far and a whole unit of the budget more for the request it
// ended in - its work was never answered, so it is charged as the most one
// request may cost, and no file reads past its budget by ending the reader -
// and asks once more. Ended a second time, the request
// fails with PstReaderFailed, and the job leaves that message or item out as
// reader_failed, or that folder, or fails the file, as it says; the next request
// starts a new reader again. MaximumFailuresInARow such failures in a row mean
// the file ends the reader on everything, and the job fails it rather than
// starting two readers for each of its messages.
//
// ONE READER A JOB, AND FEW AT ONCE. A job's phase has one reader at a time, its
// restarts included, and the server runs at most MaximumReaders at once: a job
// that finds them all taken waits for one, asking its stop meanwhile.
//
// THE TEST HOOK. ReadTestHook reads "<the PST's name>.reader-test" beside the
// PST, if an administrator has put one there: lines "end-at 0x<nid>" and
// "end-once-at 0x<nid>". The reader ends itself when asked for such a message,
// item or folder - every time, or only the first time in the phase - as a crash
// would, which is how the regression fixture shows the service surviving one.
// The job reports it (HM7553) whenever it reads one, so it cannot be on unseen.

#pragma once

#include "PstReaderWire.h"
#include "PstSource.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace HM
{
   // The reader ended twice on one request: what() says how, for the report.
   // Not a PstError, so that no catch of damage in the file takes it; a
   // std::runtime_error, so that one no catch names is still a failure of the
   // message rather than of the service.
   class PstReaderFailed : public std::runtime_error
   {
   public:
      explicit PstReaderFailed(const std::string &what) : std::runtime_error(what) { }
   };

   // No reader could be started: the job pauses, and tries again later.
   class PstReaderUnavailable : public std::runtime_error
   {
   public:
      explicit PstReaderUnavailable(const std::string &what) : std::runtime_error(what) { }
   };

   class PstReaderProcess
   {
   public:
      // The test hook's NIDs (the header).
      struct TestHook
      {
         std::vector<uint32_t> always;
         std::vector<uint32_t> once;
      };

      // source is the file the job opened and checked; stop is the job's; label
      // names the job and the file in what the error log is told; restarted is
      // called on the job's thread each time a reader is started after one
      // ended, for the job's report.
      PstReaderProcess(std::shared_ptr<PstFileSource> source, const std::function<bool()> &stop, const String &label,
                       const std::function<void()> &restarted, const TestHook &hook);
      ~PstReaderProcess();

      PstReaderProcess(const PstReaderProcess &) = delete;
      PstReaderProcess &operator=(const PstReaderProcess &) = delete;

      // PstStore's, each done in the reader: what it threw there - PstRefusal,
      // PstError, PstTooLarge, PstBudgetExhausted, std::bad_alloc - is thrown
      // here, and besides PstStopped (a stop), PstReaderFailed and
      // PstReaderUnavailable.
      void Open(const std::string &password);
      const std::string &GetRecordKey() const { return open_.recordKey; }
      const std::string &GetDisplayName() const { return open_.displayName; }
      int GetVersion() const { return (int) open_.version; }
      int GetCryptMethod() const { return (int) open_.crypt; }

      void ListFolders(std::vector<PstFolderInfo> &folders, std::vector<std::string> &problems);
      void ListMessages(uint32_t folderNid, std::vector<uint32_t> &messages);

      // Read whole, taken as a message where it is a note or a journal entry,
      // and rebuilt as Internet mail (PstMime::Build) with stem the store's and
      // the message's, as the mail phase built it in the service.
      void ReadMail(uint32_t nid, PstMailRead &mail);

      // The root folder's contacts, calendar and tasks folders, and the
      // name-to-id map read; kept, and read again in any reader started later.
      void ReadItemFolders(uint32_t &contacts, uint32_t &calendar, uint32_t &tasks);
      const std::string &NamesProblem() const { return names_problem_; }

      void ReadContact(uint32_t nid, PstContactRead &contact);
      void ReadCalendarItem(uint32_t nid, bool taskFolder, PstCalendarRead &item);

      // Whether the whole file's work budget is spent, as the last reply said.
      bool FileExhausted() const { return file_exhausted_; }

      // Requests failed with PstReaderFailed since the last one answered.
      int FailuresInARow() const { return failures_in_a_row_; }

      // "<file>.reader-test" beside the file, inside directory: false when there
      // is none, or it names nothing; said is the sentence to report.
      static bool ReadTestHook(const String &file, const String &directory, TestHook &hook, String &said);

      // What a reply's status throws in the service: nothing for one that was
      // done, else what the reader threw, as that type. Public for
      // PstReaderWireTester.
      static void Rethrow(const PstReplyStatus &status);

      static const int MaximumReaders = 4;
      static const int MaximumFailuresInARow = 10;
      static const unsigned int HelloMilliseconds = 60000;
      static const unsigned int RequestMilliseconds = 600000;

   private:
      enum Exchange
      {
         ExchangeAnswered,
         ExchangeEnded,
         ExchangeStopped
      };

      enum KilledFor
      {
         KilledForNothing = 0,
         KilledForStop = 1,
         KilledForTime = 2
      };

      typedef std::function<void(PstWireReader &reader, std::string &payload)> BodyReader;

      // One request, answered and its body read; the reader started, or
      // started again, as needed; once more after an end, then PstReaderFailed.
      // nid is the message, item or folder asked for, or 0.
      void Call_(unsigned kind, const std::string &request, const std::string &what, uint32_t nid, const BodyReader &body);

      // One frame sent (none for the hello, kind 0) and one read back.
      Exchange Exchange_(unsigned kind, const std::string &request, unsigned int milliseconds, std::string &payload, std::string &why);

      // A reader started and said hello, and the store opened in it again as
      // before; false with why when it ended in that.
      bool Start_(std::string &why);

      // The open request: the password, what the file has cost so far and
      // the test hook's NIDs.
      std::string OpenRequest_() const;
      bool Spawn_(bool lowered, bool &loweringRefused, std::string &why);
      bool Replay_(std::string &why);

      // The reader's pipes closed, the process waited for (killed after five
      // seconds), and how it ended.
      void Discard_(std::string &how);

      bool HasProcess_() const;
      void Kill_();                       // under mutex_
      bool Write_(const std::string &data);
      long long Read_(char *buffer, size_t length);
      void Arm_(unsigned int milliseconds);
      int Disarm_();
      void Watch_();

      void TakeSlot_();
      void GiveSlot_();

      void Report_(const std::string &what, const std::string &why);

      std::shared_ptr<PstFileSource> source_;
      std::function<bool()> stop_;
      String label_;
      std::function<void()> restarted_;
      TestHook hook_;

      std::string password_;
      bool opened_ = false;               // the store is open: opened again in a new reader
      bool item_folders_read_ = false;    // and its item folders and names read
      PstOpenReply open_;
      std::string names_problem_;
      bool file_exhausted_ = false;
      uint64_t items_used_ = 0;
      uint64_t bytes_used_ = 0;
      int failures_in_a_row_ = 0;
      int ends_reported_ = 0;
      bool unavailable_reported_ = false;
      bool has_slot_ = false;
      bool started_before_ = false;
      bool running_ = false;              // a reader is up and has said hello

      // The process, and the watchdog's view of the request in flight, under
      // mutex_; the pipes are the job's thread's alone.
      std::mutex mutex_;
      std::condition_variable wake_;
      std::thread watchdog_;
      bool closing_ = false;
      bool in_request_ = false;
      unsigned long long deadline_ = 0;
      int killed_for_ = KilledForNothing;
#ifdef _WIN32
      HANDLE process_ = NULL;
      HANDLE job_ = NULL;
      HANDLE input_ = NULL;
      HANDLE output_ = NULL;
      HANDLE thread_ = NULL;              // the job's thread, whose read a kill cancels
#else
      int pid_ = -1;
      int input_ = -1;
      int output_ = -1;
#endif
   };
}
