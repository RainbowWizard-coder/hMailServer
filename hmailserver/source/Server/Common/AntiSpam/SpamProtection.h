// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

#include <atomic>
#include <memory>

namespace HM
{
   class Message;
   class MessageData;

   class SpamTestRunner;
   class SpamTestResult;
   class AuthenticationResults;

   class SpamProtection : public Singleton<SpamProtection>
   {
   public:
      SpamProtection(void);
      ~SpamProtection(void);

      void Load();

      // authenticationResults is where the tests record what they concluded, for the
      // RFC 8601 header. It is defaulted and null-checked everywhere, so with the
      // feature off nothing is allocated and no test does anything extra.
      //
      // It spans both phases deliberately: SPF is a pre-transmission test while DKIM
      // and DMARC are post-transmission, so a carrier scoped to one phase could only
      // ever describe part of the answer. The connection owns it for the length of the
      // message.
      //
      // recordedReturnPath is given for a message collected from an external
      // account: the path of its topmost Return-Path, which the SpamAssassin scan is
      // sent in place of sFromAddress (SpamTestData::GetRecordedReturnPath). Empty
      // for every other message.
      std::set<std::shared_ptr<SpamTestResult> > RunPreTransmissionTests(const String &sFromAddress, const IPAddress & iOriginatingIP, const IPAddress &iConnectingIP, const String &sHeloHost, std::shared_ptr<AuthenticationResults> authenticationResults = nullptr);
      std::set<std::shared_ptr<SpamTestResult> > RunPostTransmissionTests(const String &sFromAddress, const IPAddress & iOriginatingIP, const IPAddress &iConnectingIP, std::shared_ptr<Message> pMessage, std::shared_ptr<AuthenticationResults> authenticationResults = nullptr, const AnsiString &recordedReturnPath = AnsiString());

      static std::shared_ptr<MessageData> AddSpamScoreHeaders(std::shared_ptr<Message> pMessage, std::set<std::shared_ptr<SpamTestResult> > setResult, bool classifiedAsSpam);

      // What greylisting decided for one recipient. Allow, Defer (451) and Reject
      // (550) are GreyListing::Verdict's; Pending is the fourth answer, given only
      // before DATA: a bypass row covers the sender but requires DKIM or DMARC,
      // which are known only once the message is in, so nothing has been decided
      // and nothing written - SMTPConnection asks again after DATA with afterData
      // true, when the answer is one of the other three.
      enum GreyListingVerdict
      {
         GreyListAllow = 1,
         GreyListDefer = 2,
         GreyListReject = 3,
         GreyListPending = 4
      };

      static GreyListingVerdict GreyListingVerdictFor(const String &sSenderAddress, const String &sRecipientAddress, const IPAddress & iRemoteIP);

      static int CalculateTotalSpamScore(std::set<std::shared_ptr<SpamTestResult> > result);

      static bool IsWhiteListed(const String &sFromAddress, const IPAddress & iIPAddress);

      // The largest message, in KB, the tests after DATA look at - the SURBL
      // test among them: AntiSpamMaxSizeKB when it is set and smaller, else
      // the MIME parser's own 80 MB. A larger message is not scanned at all
      // (RunPostTransmissionTests); the sender lists' URL entries read the
      // links of a larger one only as far as this into its text and its HTML
      // (SenderListCache::ReadUrlLinks).
      static int MaxSizeToScanKB();

      GreyListingVerdict PerformGreyListing(std::shared_ptr<Message> message, const std::set<std::shared_ptr<SpamTestResult> > &spamTestResults, const String &toAddress, const IPAddress &ipaddress, const String &heloHost, bool afterData);
   private:

      // The runner a run of the tests is to use, taken once per run.
      std::shared_ptr<SpamTestRunner> CurrentRunner_() const;

      // The runner and the tests it goes through. Load puts a new one here at
      // every start of the servers, a reinitialise's among them, while a thread
      // the stop before it went on without (WorkQueue::Stop) may still be in the
      // tests of the one it replaces - a session's SPF check on one of the TCP/IP
      // threads, a scan of a message being finalised. Each run takes its own
      // reference first (CurrentRunner_), so that the runner it began with, and
      // its tests, live until it has finished. Read and replaced atomically, as
      // IniFileSettings' block is.
#if defined(__cpp_lib_atomic_shared_ptr)
      std::atomic<std::shared_ptr<SpamTestRunner>> spam_test_runner_;
#else
      std::shared_ptr<SpamTestRunner> spam_test_runner_;
#endif
   };

}