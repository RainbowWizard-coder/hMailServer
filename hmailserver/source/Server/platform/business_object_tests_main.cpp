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
//   - A message's X-hMailServer-LoopCount, which comes with mail from anyone, is a
//     count only when it is digits, and never above the limit (LoopCountTester,
//     Common/Rules/RuleGuard.cpp): MessageData and the Sieve evaluator read it with
//     _ttoi until 6 October 2026, so "-5" passed every RuleLoopLimit test and
//     "2147483647" overflowed the count + 1 a generated message carries.
//   - And no edit resets it (LoopCountEditTester, the same file): a Sieve script's
//     addheader or deleteheader of it is refused when the script is stored and not
//     made when a stored one runs, by name in any case and as the script's variables
//     made it; a rule may not set it; and a line break in a value a rule or a script
//     writes, which wrote it as a second field, is a fold or a space.
//   - A Sieve vacation reply or notification says it is from its :from only when
//     that address reaches the account, and otherwise from the account's own
//     address (SieveFromAddressTester, Common/Sieve/SieveVacationResponder.cpp):
//     until 6 October 2026 a notification was sent from whatever :from said. A
//     notify :from that is not an address is refused when the script is stored, as
//     vacation's is, and a script stored before still runs. And a redirect says it
//     is from what the message said: an addheader or deleteheader of From, Sender,
//     the Resent- pair or DKIM-Signature is refused when stored and not made when a
//     stored script runs.
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
#include "../Common/BO/Message.h"
#include "../Common/Diagnostics/DiagnosticResult.h"
#include "../Common/Rules/RuleGuard.h"
#include "../Common/Sieve/SieveVacationResponder.h"
#include "../Common/Sieve/ManageSieveServer.h"
#include "../Common/Tracking/ChangeNotification.h"
#include "../Common/Threading/WorkQueue.h"
#include "../Common/Scripting/ScriptObjectContainer.h"
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
         return 0;
      }
      catch (const std::exception &failure)
      {
         printf("FAIL  %s: %s\n", name, failure.what());
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
   failed += Run(&TestFetchAccountRetention, "an external account's DaysToKeepMessages: one meaning for POP3 and IMAP, and what a value stored before schema 6106 or archived before 6.3.6 means in it");
   failed += Run(&TestMessageCopy, "a message's copy: every member it carries, and the one it leaves out");
   failed += Run(&TestDiagnosticResult, "a DiagnosticResult made empty is not a success");
   failed += Run(&TestChangeNotification, "a ChangeNotification has no account, folder or data it was not given");
   failed += Run(&TestWorkQueueTaskInfo, "a WorkQueueTaskInfo starts with every field zero");
   failed += Run(&TestOidcMapping, "an OidcSignIn::Mapping starts with no switch");
   failed += Run(&TestScriptObject, "a script object is made with its name and type");
   failed += Run(&TestIpRangePriority, "the IP range's priority a create takes: PUT's whole numbers from -2,000,000,000 to 2,000,000,000");
   failed += Run(&TestLoopCount, "a message's X-hMailServer-LoopCount: digits up to the limit, and anything else the limit, never below 0 or past INT_MAX - 1");
   failed += Run(&TestLoopCountEdits, "no Sieve edit, rule or line break in a header value resets X-hMailServer-LoopCount");
   failed += Run(&TestSieveFromAddress, "a Sieve vacation reply or notification is from its :from only when that reaches the account, and no Sieve edit changes who a message is from");
   failed += Run(&TestManageSieveQuoting, "a ManageSieve quoted string holds no line break: a refusal quoting a script's line with one in it is one line");

   return failed == 0 ? 0 : 1;
}
