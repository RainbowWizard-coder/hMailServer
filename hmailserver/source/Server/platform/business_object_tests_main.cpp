// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The business objects' copies and first values, and the priority REST creates an
// IP range with, as a program of their own for the POSIX build - the counterpart
// of spf-tests and datetime-tests.
//
// On Windows AccountTester runs inside the service with the other class tests
// (ClassTester, which Utilities.RunTestSuite reaches), and the regression suite
// covers the rest through COM and REST (MessageRetentionTests, RestApiCoverage,
// the IMAP suite). That route does not exist here, so the CMake build makes this
// and linux-build.sh runs it with every other *-tests program.
//
// What it holds, each found by the 6.3.6 static-analysis triage:
//
//   - Account's copy constructor copies every member. It is what COM's
//     Authenticate hands a webmail, which saves it; from 6.2.25 to 6.3.5 it left
//     the account's message retention unset and dropped its mailbox key wrap.
//     AccountTester (Common/BO/Account.cpp) copies an account with every field
//     set into memory filled with a pattern first and compares every getter.
//   - Message's copy constructor carries every member it is meant to, its id
//     through its base, and leaves out what it is meant to.
//   - DiagnosticResult, ChangeNotification, WorkQueueTaskInfo and
//     OidcSignIn::Mapping start with every scalar set, even in memory that held
//     something else - made there over a pattern, so that a member left unset
//     reads as the pattern rather than as whatever the heap had.
//   - A ScriptObject is made with its type.
//   - RestApiServer::ReadIpRangePriority holds a POST /api/v1/ipranges priority to
//     what PUT takes: a whole number from -2,000,000,000 to 2,000,000,000.
//   - An external account's DaysToKeepMessages has one meaning for POP3 and IMAP
//     (FetchAccountTester, Common/BO/FetchAccount.cpp): from 6.2.25 to 6.3.5 the
//     IMAP fetcher read 0 and -1 the other way round. What a value stored before
//     schema 6106 means in it, and an archive written before 6.3.6 read through
//     that rule while one written since is read as it is.
//   - A scheduled task due at a second is due at a turn of the Scheduler within it
//     (ScheduledTaskTester, Common/BO/ScheduledTask.cpp): until 9 October 2026 its
//     next run was a local DATE moved on by a minute's span, which came out a
//     rounding error above the second it meant, and the task - the minute's
//     RemoveExpiredRecords among them - waited a whole turn more.
//   - The mailbox keys' idle sweep closes no key in use (MailboxKeyIdleTester,
//     Common/Util/MailboxKeys.cpp): a key used in the clock's next second while a
//     sweep waits for the keys' lock stays open. Until 10 October 2026 a sweep read
//     the clock before it took the lock, and such a use read as a clock gone back,
//     which closes the key.
//   - An exception on one of the server's own threads is answered as a failure and
//     ends no more than that thread (ExceptionHandler::Run, Common/Application/
//     ExceptionHandler.cpp): until 10 October 2026 it was passed on, on POSIX, to
//     std::terminate. Run late, since it stops the log's writes for the rest of the
//     program (the handler reports the exception, HM4208).
//   - The message indexer's thread is waited for when it is stopped, and a Start
//     straight after a Stop begins a new one (MessageIndexerTester, Common/
//     Application/MessageIndexer.cpp, over a stand-in pass with no database):
//     until 10 October 2026 a Stop interrupted and returned, a pass in the middle
//     of a batch went on to the database a Reinitialize had closed - the Linux
//     server ended there, in the Alpine suite - and the Start after it took the
//     ending thread for the running indexer and started none. And an exception on
//     its thread ends that thread alone, and the next Start begins it again (after
//     the check above, as it reports the exception too). And a Start on the
//     indexer's own thread, as an OnError script makes one, neither asks the
//     thread to join itself - boost threw, until 10 October 2026 - nor ends the
//     indexer when it follows a Stop there. A Stop with a limit, as StopServers
//     makes one before the process ends, gives up on a pass that outlasts it;
//     and a Stop and a Start the indexer's own thread makes while a Stop waits
//     for it return at once, where the wait held the lock they take. And a Stop
//     ends a pass that swallowed its request to stop in a catch (...): until 10
//     October 2026 the request was asked once, and a Reinitialize waited for ever.
//     The servers' mutex, held by a Stop waiting for the indexer, is taken on the
//     indexer's own thread only when free: an OnError script's Reinitialize
//     waited for it for ever while another reinitialise held it.
//     A tester's stand-in, which uses no database, starts while the servers are
//     stopped, where ClassTester's indexer tests in such a server each waited a
//     minute and failed.
//   - The servers' mutex, which every stop, start and reinitialise of the servers
//     holds, refuses what would wait for ever (ServersLockTester, Common/
//     Application/Application.cpp, through Application itself): a reinitialise, a
//     stop or a start asked on the thread that holds it, by a script the stop or
//     the start runs, or on the message indexer's own thread while a stop that
//     holds it waits for the indexer. A reinitialise asked on the indexer's own
//     thread is refused with the mutex free as well, touching nothing: until the
//     release review of 6.3.7 it was let through, its stop asked that thread to
//     end, and the join of the maintenance queue's threads after it met the
//     request, the servers left stopped, the server unconfigured and the queue
//     stopped but registered. Once the stop before the process ends has
//     begun every one is refused, and that stop waits for one under way, where
//     until 10 October 2026 it closed the database under a reinitialise. It
//     closes every database connection but one in use, which it leaves open for
//     its caller, where it closed it under the statement, and answers that the
//     process is to end at once while a connection is in use or a thread a work
//     queue's stop gave up on still runs - and only while it runs, where from 10
//     October 2026 until the re-check of that change it counted a queue that had
//     once given up on a thread for the rest of the process. COM's Stop and
//     Start wait five seconds at most for one under way, where they waited for
//     it without a limit, and so does an event script's reinitialise, whose
//     thread what holds the mutex may be joining; and the service's start takes
//     the mutex before it opens the database. Run last, since it makes that
//     stop.
//   - The database's close before the process ends (DatabaseConnectionManager-
//     Tester, Common/SQL/DatabaseConnectionManager.cpp, over stand-in
//     connections): an idle connection is closed at once and one in use only
//     when its caller hands it back, nothing is handed out after the close, and
//     the wait for one in use ends when it comes back or at its limit - SQL
//     Server Compact's flush interval and two seconds, nothing on a database
//     server.
//   - A message's X-hMailServer-LoopCount, which comes with mail from anyone, is a
//     count only when it is digits, and never above the limit (LoopCountTester,
//     Common/Rules/RuleGuard.cpp): MessageData and the Sieve evaluator read it with
//     _ttoi until 6 October 2026, so "-5" passed every RuleLoopLimit test and
//     "2147483647" overflowed the count + 1 a generated message carries.
//   - And no edit resets it (LoopCountEditTester, the same file): a Sieve script's
//     addheader or deleteheader of it is refused when the script is stored and not
//     made when a stored one runs, by name in any case and as the script's variables
//     made it; a rule may not set it; and a line break in a value a rule or a script
//     writes, which wrote it as a second field, is a fold or a space. A script's edit
//     of Auto-Submitted is held the same way (RFC 5293 section 6).
//   - A rule's actions are held to who owns the rule (RuleOwnerTester, the same file):
//     a domain's or an account's rule may not set From, Sender, the Resent- pair or
//     DKIM-Signature, a reply goes only from an address the rule's owner may send as,
//     and a reply's From reads as the one address whatever its name says - until 7
//     October 2026 an account's rule could send its reply, or forward a message, as
//     anyone, signed with the DKIM key of the domain it named. And a Run function
//     action is stored only naming a function the server would run - a plain name,
//     the firing path's own test - where until 12 October 2026 COM stored any name
//     and the REST routes any but an empty one, and the server refused it only each
//     time the rule fired.
//   - DKIM signs a message as the domain its From names only when the server is
//     entitled to (SigningOriginTester, Common/AntiSpam/DKIM/DKIMSigner.cpp): not mail
//     from outside, nor a forward, a redirect or a rule's copy of it, nor a signed-in
//     submission whose From names another hosted domain the account may not send as -
//     the mark that says so is carried by every copy and kept on the queue row, as is
//     the hosted domain recorded as relaying a message, which seals the ARC set over
//     it. Until 7 October 2026 a forward of mail from outside whose From named a
//     hosted domain left this server signed as that domain.
//   - The spf= of the ARC-Authentication-Results this server seals is its own SPF
//     result for the message (ArcAuthenticationResultsTester, Common/AntiSpam/DKIM/
//     Arc.cpp), carried by every copy and kept on the queue row and in a quarantined
//     copy's name; until 7 October 2026 it was read from the first Received-SPF field,
//     which anybody could write. The value names header.d for each signature that
//     verified and the domain the SPF check evaluated as smtp.mailfrom.
//   - A Sieve vacation reply or notification says it is from its :from only when
//     that address reaches the account, and otherwise from the account's own
//     address (SieveFromAddressTester, Common/Sieve/SieveVacationResponder.cpp):
//     until 6 October 2026 a notification was sent from whatever :from said. A
//     notify :from that is not an address is refused when the script is stored, as
//     vacation's is, and a script stored before still runs. And a redirect says it
//     is from what the message said: an addheader or deleteheader of From, Sender,
//     the Resent- pair or DKIM-Signature is refused when stored and not made when a
//     stored script runs.
//   - A copy of mail from outside is given an envelope sender of its From's
//     organisation only when the message passed DMARC for it when it arrived, and a
//     copy is not sent outside with a null sender under a host name of that
//     organisation, nor with the envelope sender it came with at this server's host
//     name or a domain hosted here (CopyEnvelopeTester, SMTP/SMTPForwarding.cpp):
//     the organisations a From names, read through the public suffix list from every
//     field, every address and every @ in one; the DMARC passes a message's record
//     holds; which envelope senders are this server's own; the choice, with and
//     without a pass, a rewrite and a null sender; and what DMARC passed for when the
//     mail arrived, which every copy carries apart from the record of the message
//     that arrived - CreateCopy_, the queue row and the quarantine's stored name.
//   - A ManageSieve quoted string holds no CR, LF or NUL (ManageSieveResponseTester,
//     Common/Sieve/ManageSieveServer.cpp): a PUTSCRIPT or CHECKSCRIPT refusal quoting
//     a script's line with a line break in it split the NO response until 7 October
//     2026.
//
// No database, configuration or network. It prints a line per check and exits
// non-zero when one fails.

#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>

#include "../Common/BO/Account.h"
#include "../Common/BO/FetchAccount.h"
#include "../Common/BO/ScheduledTask.h"
#include "../Common/BO/Message.h"
#include "../Common/AntiSpam/DKIM/DKIMSigner.h"
#include "../Common/AntiSpam/DKIM/Arc.h"
#include "../SMTP/SMTPForwarding.h"
#include "../Common/Diagnostics/DiagnosticResult.h"
#include "../Common/Rules/RuleGuard.h"
#include "../Common/Sieve/SieveVacationResponder.h"
#include "../Common/Sieve/ManageSieveServer.h"
#include "../Common/Tracking/ChangeNotification.h"
#include "../Common/Threading/WorkQueue.h"
#include "../Common/Scripting/ScriptObjectContainer.h"
#include "../Common/Util/MailboxKeys.h"
#include "../Common/Application/ExceptionHandler.h"
#include "../Common/Application/Logger.h"
#include "../Common/Application/Application.h"
#include "../Common/SQL/DatabaseConnectionManager.h"
#include "../Common/Application/MessageIndexer.h"
#include "../Common/Util/OidcSignIn.h"
#include "../Common/Util/RestApiServer.h"

namespace
{
   void
   Expect(bool condition, const std::string &what)
   {
      if (!condition)
         throw std::logic_error(what);
   }

   // Storage for one T, filled with a byte before the object is made in it and
   // destroyed with it. A member the constructor leaves unset keeps the byte.
   template <class T>
   class Patterned
   {
   public:
      explicit Patterned(unsigned char pattern)
      {
         memset(storage_, pattern, sizeof(storage_));
      }

      ~Patterned()
      {
         if (object_)
            object_->~T();
      }

      Patterned(const Patterned &) = delete;
      Patterned &operator=(const Patterned &) = delete;

      void *Place() { return static_cast<void *>(storage_); }
      void Hold(T *object) { object_ = object; }
      T &Get() const { return *object_; }

   private:
      alignas(T) unsigned char storage_[sizeof(T)] = {};
      T *object_ = nullptr;
   };

   const unsigned char Patterns[] = { 0x00, 0xA5, 0xFF };

   void
   TestMessageCopy()
   {
      HM::Message source(false);
      source.SetID(4712);
      source.SetSize(12345);
      source.SetCreateTime(_T("2030-01-02 03:04:05"));
      source.SetSaveDate(_T("2030-01-02 03:04:06"));
      source.SetEmailId(_T("M0123456789abcdef"));
      source.SetPartialFileName(_T("{00000000-0000-0000-0000-000000004712}.eml"));
      source.SetFromAddress(_T("sender@example.test"));
      source.SetAccountID(17);
      source.SetFolderID(23);
      source.SetState(HM::Message::Delivered);
      source.SetNoOfRetries(3);
      source.SetFlags(0x25);
      source.SetKeywords(_T("$Label1 project"));
      source.SetUID(99);
      source.SetModSeq(123456789012LL);
      source.SetSubmitterAccountID(29);
      source.SetAuthDomains(_T("example.test"));
      source.SetCopiedAuthDomains(_T("dmarc=partner.example"));
      source.SetFromOutside(true);
      source.SetReceivedSpfResult(_T("softfail"));
      source.SetReceivedSpfDomain(_T("bounces.example.test"));
      source.SetRelayingDomainID(37);
      source.SetCopiedFromID(31);

      for (unsigned char pattern : Patterns)
      {
         Patterned<HM::Message> place(pattern);
         place.Hold(new (place.Place()) HM::Message(source));
         const HM::Message &copy = place.Get();

         Expect(copy.GetID() == 4712, "a copy of a message lost its id");
         Expect(copy.GetSize() == source.GetSize(), "a copy of a message lost its size");
         Expect(copy.GetCreateTime() == source.GetCreateTime(), "a copy of a message lost its create time");
         Expect(copy.GetSaveDate() == source.GetSaveDate(), "a copy of a message lost its save date");
         Expect(copy.GetEmailId() == source.GetEmailId(), "a copy of a message lost its EMAILID");
         Expect(copy.GetPartialFileName() == source.GetPartialFileName(), "a copy of a message lost its file name");
         Expect(copy.GetFromAddress() == source.GetFromAddress(), "a copy of a message lost its sender");
         Expect(copy.GetAccountID() == source.GetAccountID(), "a copy of a message lost its account");
         Expect(copy.GetFolderID() == source.GetFolderID(), "a copy of a message lost its folder");
         Expect(copy.GetState() == source.GetState(), "a copy of a message lost its state");
         Expect(copy.GetNoOfRetries() == source.GetNoOfRetries(), "a copy of a message lost its retry count");
         Expect(copy.GetFlags() == source.GetFlags(), "a copy of a message lost its flags");
         Expect(copy.GetKeywords() == source.GetKeywords(), "a copy of a message lost its keywords");
         Expect(copy.GetUID() == source.GetUID(), "a copy of a message lost its UID");
         Expect(copy.GetModSeq() == source.GetModSeq(), "a copy of a message lost its mod-sequence");
         Expect(copy.GetSubmitterAccountID() == source.GetSubmitterAccountID(), "a copy of a message lost its submitter");
         Expect(copy.GetAuthDomains() == source.GetAuthDomains(), "a copy of a message lost its authenticated domains");
         Expect(copy.GetCopiedAuthDomains() == source.GetCopiedAuthDomains(), "a copy of a message lost what DMARC passed for when the mail it was made from arrived");
         Expect(copy.GetFromOutside(), "a copy of a message from outside lost the mark that says so");
         Expect(copy.GetReceivedSpfResult() == source.GetReceivedSpfResult(), "a copy of a message lost the SPF result this server reached for it");
         Expect(copy.GetReceivedSpfDomain() == source.GetReceivedSpfDomain(), "a copy of a message lost the domain its SPF check evaluated");
         Expect(copy.GetRelayingDomainID() == 37, "a copy of a message lost the domain recorded as relaying it");

         // Left out on purpose (Message.cpp): the recall rows follow the one
         // object CopyToIMAPFolder saves, never a copy of it.
         Expect(copy.GetCopiedFromID() == 0, "a copy of a message carried the id it was copied from");
      }
   }

   void
   TestDiagnosticResult()
   {
      for (unsigned char pattern : Patterns)
      {
         Patterned<HM::DiagnosticResult> place(pattern);
         place.Hold(new (place.Place()) HM::DiagnosticResult());
         Expect(!place.Get().GetSuccess(), "a DiagnosticResult made empty reads as a success");
      }
   }

   void
   TestChangeNotification()
   {
      for (unsigned char pattern : Patterns)
      {
         {
            Patterned<HM::ChangeNotification> place(pattern);
            place.Hold(new (place.Place()) HM::ChangeNotification(HM::ChangeNotification::NotificationMessageAdded));
            const HM::ChangeNotification &notification = place.Get();
            Expect(notification.GetAccountID() == 0 && notification.GetFolderID() == 0 && notification.GetData() == 0,
                   "a ChangeNotification made with a type alone has an account, a folder or data");
         }

         {
            Patterned<HM::ChangeNotification> place(pattern);
            place.Hold(new (place.Place()) HM::ChangeNotification(3, 5, HM::ChangeNotification::NotificationMessageDeleted));
            const HM::ChangeNotification &notification = place.Get();
            Expect(notification.GetAccountID() == 3 && notification.GetFolderID() == 5 && notification.GetData() == 0,
                   "a ChangeNotification made without data has data");
         }
      }
   }

   void
   TestWorkQueueTaskInfo()
   {
      for (unsigned char pattern : Patterns)
      {
         // Default-initialised, as WorkQueue::GetRunningTasks makes each entry
         // before it fills it.
         Patterned<HM::WorkQueueTaskInfo> place(pattern);
         place.Hold(new (place.Place()) HM::WorkQueueTaskInfo);
         const HM::WorkQueueTaskInfo &info = place.Get();
         Expect(info.thread_id == 0 && info.queue_wait_ms == 0 && info.running_ms == 0 && !info.may_block,
                "a WorkQueueTaskInfo starts with a value in a field it was not given");
      }
   }

   void
   TestOidcMapping()
   {
      for (unsigned char pattern : Patterns)
      {
         // Default-initialised, as OidcSignIn and SamlSignIn make theirs.
         Patterned<HM::OidcSignIn::Mapping> place(pattern);
         place.Hold(new (place.Place()) HM::OidcSignIn::Mapping);
         Expect(place.Get().auto_provision == nullptr, "an OidcSignIn::Mapping starts with a switch it was not given");
      }
   }

   void
   TestScriptObject()
   {
      HM::ScriptObjectContainer container;
      container.AddObject(_T("HMAILSERVER_CLIENT"), HM::ScriptObject::OTClient);
      std::shared_ptr<HM::ScriptObject> object = container.FindObject(_T("HMAILSERVER_CLIENT"));
      Expect(object && object->eType == HM::ScriptObject::OTClient && !object->pObject && object->sName == _T("HMAILSERVER_CLIENT"),
             "a script object is not made as it was added");
   }

   void
   TestIpRangePriority()
   {
      struct Case
      {
         const char *body;
         bool accepted;
         long priority;
      };

      // 17 is what priority holds before the call: a body without the member,
      // or with null there, leaves it, as the create leaves its 0.
      const Case cases[] =
      {
         { "{\"name\":\"r\",\"priority\":42}", true, 42 },
         { "{\"priority\": -7, \"name\":\"r\"}", true, -7 },
         { "{\"priority\":0}", true, 0 },
         { "{\"priority\":2000000000}", true, 2000000000L },
         { "{\"priority\":-2000000000}", true, -2000000000L },
         { "{\"priority\":42.0}", true, 42 },
         { "{\"priority\":1e3}", true, 1000 },
         { "{\"priority\":\r\n  5}", true, 5 },
         { "{\"name\":\"r\"}", true, 17 },
         { "{\"priority\":null}", true, 17 },
         // A range named "priority" is not read as its priority: the member is
         // found by name in the parsed document, as the name itself is.
         { "{\"name\":\"priority\",\"lower\":\"10.0.0.1\",\"upper\":\"10.0.0.2\",\"priority\":-7}", true, -7 },
         { "{\"name\":\"priority\",\"lower\":\"10.0.0.1\",\"upper\":\"10.0.0.2\",\"priority\":99999999999}", false, 17 },
         // Not a number: refused, as PUT refuses it.
         { "{\"priority\":\"5\"}", false, 17 },
         { "{\"priority\":true}", false, 17 },
         // A body that does not parse (the comma before the brace): the number
         // after the key, held to the same bound; nothing there leaves it.
         { "{\"priority\":5,}", true, 5 },
         { "{\"priority\":99999999999,}", false, 17 },
         { "{\"priority\":\"5\",}", true, 17 },
         { "{\"priority\":2000000001}", false, 17 },
         { "{\"priority\":-2000000001}", false, 17 },
         { "{\"priority\":99999999999}", false, 17 },
         { "{\"priority\":4294967338}", false, 17 },
         { "{\"priority\":123456789012345678901234567890}", false, 17 },
         { "{\"priority\":-}", false, 17 },
         { "{\"priority\":1.5}", false, 17 },
         { "{\"priority\":+5}", false, 17 },
         { "{\"priority\":1e999}", false, 17 },
      };

      // Every case judged before the answer, so that a failure names each body
      // read wrongly and not only the first.
      std::string wrong;
      for (const Case &c : cases)
      {
         long priority = 17;
         const bool accepted = HM::RestApiServer::ReadIpRangePriority(c.body, priority);
         if (accepted != c.accepted || priority != c.priority)
            wrong += std::string(wrong.empty() ? "" : "; ") + c.body + (accepted ? " was taken as " + std::to_string(priority) : std::string(" was refused"));
      }
      Expect(wrong.empty(), "the priority of " + wrong);
   }

   int
   Run(void (*test)(), const char *name)
   {
      try
      {
         test();
         printf("ok    %s\n", name);
         (void) fflush(stdout);
         return 0;
      }
      catch (const std::exception &failure)
      {
         // Written out at once, so that a test after it that never returns
         // leaves the failure in the output.
         printf("FAIL  %s: %s\n", name, failure.what());
         (void) fflush(stdout);
         return 1;
      }
   }

   void
   TestAccountCopy()
   {
      HM::AccountTester tester;
      tester.Test();
   }

   void
   TestFetchAccountRetention()
   {
      HM::FetchAccountTester tester;
      tester.Test();
   }

   void
   TestScheduledTaskTimes()
   {
      HM::ScheduledTaskTester tester;
      tester.Test();
   }

   void
   TestMailboxKeyIdleSweep()
   {
      HM::MailboxKeyIdleTester tester;
      tester.Test();
   }

   void
   TestWorkerException()
   {
      // The handler reports what the work throws, and this program writes no
      // log: a report counted, not written.
      HM::Logger::ForbidWritesForThisProcess();
      const long before = HM::Logger::SuppressedWrites();

      // On a thread of its own, as the server's workers run it: an exception
      // that left the handler would leave the thread, which is std::terminate.
      bool answered = true;
      boost::thread worker([&answered]()
      {
         boost::function<void()> fails = []() { throw std::runtime_error("a worker's unexpected error"); };
         answered = HM::ExceptionHandler::Run("business-object-tests", fails);
      });
      worker.join();

      Expect(!answered, "ExceptionHandler::Run answered that work which threw had run");
      Expect(HM::Logger::SuppressedWrites() > before, "ExceptionHandler::Run did not report the exception");
   }

   void
   TestMessageIndexerStopAndStart()
   {
      HM::MessageIndexerTester tester;
      tester.Test();
      tester.TestAStandInStartsWhileTheServersAreStopped();
   }

   void
   TestMessageIndexerException()
   {
      // The log's writes are stopped already (TestWorkerException).
      const long before = HM::Logger::SuppressedWrites();

      HM::MessageIndexerTester tester;
      tester.TestAnExceptionOnItsThread();

      Expect(HM::Logger::SuppressedWrites() > before, "the exception on the indexer's thread was not reported");
   }

   void
   TestServersLock()
   {
      HM::ServersLockTester tester;
      tester.Test();
   }

   void
   TestServersLockBeforeExit()
   {
      HM::ServersLockTester tester;
      tester.TestTheStopBeforeExit();
   }

   void
   TestDatabaseCloseBeforeExit()
   {
      HM::DatabaseConnectionManagerTester tester;
      tester.Test();
   }

   void
   TestLoopCount()
   {
      HM::LoopCountTester tester;
      tester.Test();
   }

   void
   TestLoopCountEdits()
   {
      HM::LoopCountEditTester tester;
      tester.Test();
   }

   void
   TestRuleOwner()
   {
      HM::RuleOwnerTester tester;
      tester.Test();
   }

   void
   TestSigningOrigin()
   {
      HM::SigningOriginTester tester;
      tester.Test();
   }

   void
   TestCopyEnvelope()
   {
      HM::CopyEnvelopeTester tester;
      tester.Test();
   }

   void
   TestArcAuthenticationResults()
   {
      HM::ArcAuthenticationResultsTester tester;
      tester.Test();
   }

   void
   TestSieveFromAddress()
   {
      HM::SieveFromAddressTester tester;
      tester.Test();
   }

   void
   TestManageSieveQuoting()
   {
      HM::ManageSieveResponseTester tester;
      tester.Test();
   }
}

int
main(int /*argc*/, char * /*argv*/[])
{
   int failed = 0;
   failed += Run(&TestAccountCopy, "an account's copy: every member, the retention and the mailbox key wrap among them");
   failed += Run(&TestScheduledTaskTimes, "a scheduled task due at a second is due at a turn within it, run after run, in whole seconds, and one late by more than its period resumes from now");
   failed += Run(&TestMailboxKeyIdleSweep, "the mailbox keys' idle sweep closes no key in use: a key used in the clock's next second while a sweep waits for the keys' lock stays open");
   failed += Run(&TestFetchAccountRetention, "an external account's DaysToKeepMessages: one meaning for POP3 and IMAP, and what a value stored before schema 6106 or archived before 6.3.6 means in it");
   failed += Run(&TestMessageCopy, "a message's copy: every member it carries, and the one it leaves out");
   failed += Run(&TestDiagnosticResult, "a DiagnosticResult made empty is not a success");
   failed += Run(&TestChangeNotification, "a ChangeNotification has no account, folder or data it was not given");
   failed += Run(&TestWorkQueueTaskInfo, "a WorkQueueTaskInfo starts with every field zero");
   failed += Run(&TestOidcMapping, "an OidcSignIn::Mapping starts with no switch");
   failed += Run(&TestScriptObject, "a script object is made with its name and type");
   failed += Run(&TestIpRangePriority, "the IP range's priority a create takes: PUT's whole numbers from -2,000,000,000 to 2,000,000,000");
   failed += Run(&TestLoopCount, "a message's X-hMailServer-LoopCount: digits up to the limit, and anything else the limit, never below 0 or past INT_MAX - 1");
   failed += Run(&TestLoopCountEdits, "no Sieve edit, rule or line break in a header value resets X-hMailServer-LoopCount, and no Sieve edit changes Auto-Submitted");
   failed += Run(&TestRuleOwner, "a rule's actions held to its owner: no From or DKIM-Signature set but by a global rule, a reply sent only from an address the owner may send as, and its From one mailbox");
   failed += Run(&TestSigningOrigin, "no message from outside, nor a forward, redirect or copy of one, nor a signed-in submission whose From is another's, is signed as its From's domain; the mark and the relaying domain are copied and kept on the queue row and in a quarantined copy's name");
   failed += Run(&TestCopyEnvelope, "a forward, a redirect, a list's or an alias's copy of mail from outside is given an envelope sender of its From's organisation only for a message that passed DMARC for it, and none is sent outside with a null sender under a host name of that organisation or with the envelope sender it came with at this server's host name or a hosted domain; and a copy carries the pass the message arrived with - a copy of a copy, a queue row and a quarantined copy's name");
   failed += Run(&TestArcAuthenticationResults, "an ARC seal's spf= is the server's own SPF result, carried by copies, the queue row and a quarantined copy's name, never a Received-SPF field the message carries, and the seal names header.d and smtp.mailfrom");
   failed += Run(&TestSieveFromAddress, "a Sieve vacation reply or notification is from its :from only when that reaches the account, and no Sieve edit changes who a message is from");
   failed += Run(&TestManageSieveQuoting, "a ManageSieve quoted string holds no line break: a refusal quoting a script's line with one in it is one line");
   failed += Run(&TestMessageIndexerStopAndStart, "the message indexer's Stop waits for the pass in flight, or gives up at its limit, a Start straight after a Stop begins a new indexer, and a Start or a Stop on the indexer's own thread neither joins it nor waits for a Stop that waits for it, and a Stop ends a pass that swallowed its request to stop; the servers' mutex a Stop holds while it waits is not waited for on the indexer's own thread; a tester's stand-in starts while the servers are stopped");
   failed += Run(&TestWorkerException, "an exception on a worker's thread is reported and answered as a failure, and ends nothing more than the work");
   failed += Run(&TestMessageIndexerException, "an exception on the message indexer's thread is reported and ends that thread, not the process, and the next Start begins the indexer again");
   failed += Run(&TestServersLock, "a reinitialise, a stop or a start asked on the thread that holds the servers' mutex, or on the message indexer's own thread while a stop that holds it waits for the indexer, is refused at once, and a reinitialise asked on the indexer's own thread is refused with the mutex free too, touching nothing");
   failed += Run(&TestDatabaseCloseBeforeExit, "the database's close before the process ends closes an idle connection at once and one in use only when it is handed back, hands out none after it, and waits out SQL Server Compact's flush interval for one in use, nothing on a database server; a connection made after it is closed, not pooled, and one the start's checks run on is held for them");

   // Last: it makes the stop before the process ends.
   failed += Run(&TestServersLockBeforeExit, "once the stop before the process ends has begun a reinitialise, a stop or a start is refused, that stop waits for one under way, closes every database connection but one in use, and ends the process at once while one is in use or a thread a work queue's stop gave up on still runs, and only while it runs; COM's Stop and Start, and an event script's reinitialise, wait a few seconds at most for one under way, and the service's start takes the mutex before it opens the database");

   return failed == 0 ? 0 : 1;
}
