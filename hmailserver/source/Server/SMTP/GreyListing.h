// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2010 Martin Knafve / hMailServer.com.

#pragma once

namespace HM
{
   class GreyListTriplet;

   // Greylisting (Evans, 2003): the first attempt by a triplet - sender, recipient
   // and connecting address - is answered 451, and an attempt after the initial
   // delay is let through and remembered. Hardened on 18 September 2026 by three
   // things an operator asked for (discussions #281, #282, #284), each off by
   // default:
   //
   //   - the triplet is keyed by a fixed-width digest of each address (schema
   //     6072; GreyListTriplet::ComputeKey), so two addresses longer than the
   //     columns used to be are two triplets - and since schema 6107 every row
   //     has its key, the key is unique, and it is the whole of the lookup;
   //   - a retry before the window opens can MOVE the window
   //     (GreyListingSlideOnEarlyRetry), so a sender that retries too soon never
   //     reaches it;
   //   - a triplet retried too soon too many times (GreyListingAbuseThreshold) is
   //     refused with 550 for a while (GreyListingAbuseLockMinutes), which is what
   //     stops a sender that retries on a timer rather than a queue.
   //
   // The bypass list, the third thing, is decided before this class is asked at
   // all: Common/AntiSpam/GreyListingBypass.h.
   class GreyListing
   {
   public:
      enum Verdict
      {
         // Let the recipient through: the triplet is known and its window has
         // passed, the address is on the white list, or the record could not be
         // written and this server will not refuse mail over its own fault.
         Allow = 1,
         // 451: the first attempt, or one before the window opens.
         Defer = 2,
         // 550: the triplet has retried too soon at least the threshold's number
         // of times, and is locked for the abuse lock's minutes.
         Reject = 3
      };

      GreyListing(void);
      ~GreyListing(void);

      Verdict GetVerdict(const String &sSenderAddress, const String &sRecipientAddress, const IPAddress & remoteIP);

   private:

      friend class GreyListingTester;

      // A triplet the lookup did not find: written, and deferred - unless the
      // write is refused because the triplet is there after all (another
      // connection wrote it after this one looked), in which case what is there
      // is read and answered as the triplet it is. A refused write is never a
      // pass; only a database that can neither write nor read the triplet lets
      // the recipient through, as every other failure of this server's own
      // does.
      Verdict VerdictForUnknown_(const String &sSenderAddress, const String &sRecipientAddress, const IPAddress &remoteIP, const String &senderKey, const String &recipientKey);

      // A triplet that is there: the window, the slide and the abuse lock.
      Verdict VerdictForKnown_(std::shared_ptr<GreyListTriplet> pTriplet, const String &sSenderAddress, const String &sRecipientAddress, const IPAddress &remoteIP);
   };

   // Holds the insert's refusal to examples, in ClassTester::DoTests, on the
   // server's own database: a triplet another connection wrote between this
   // connection's lookup and its insert - confirmed, pending, or not there at
   // all - is answered as the triplet it is. See the definition.
   class GreyListingTester
   {
   public:
      void Test();
   };
}
