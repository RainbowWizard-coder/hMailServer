// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "VirusScanningResult.h"

namespace HM
{
   class Message;

   class VirusScanner  
   {
   public:
   
      // Guard for the MaxRunningScanners cap. It tries to take a scanner slot when
      // it is constructed and gives the slot back when it is destroyed - but only
      // if it actually got one.
      //
      // Owning the "I hold a slot" state here is the whole point. Previously the
      // destructor decremented the counter unconditionally, while the give-up
      // paths in WaitForFreeScanner_ returned without having incremented it. Every
      // give-up therefore drove the counter one lower until it went negative, and
      // once it was negative the cap stopped limiting anything at all - which is
      // exactly the protection that keeps a slow scanner from fanning out across
      // the whole thread pool.
      class VirusScannerAutoCount
      {
      public:
         VirusScannerAutoCount();
         ~VirusScannerAutoCount();

         // True if this object holds one of the MaxRunningScanners slots. False
         // means we gave up waiting and the scan is running over the cap.
         bool GetHoldsSlot() const;

      private:

         // Not copyable - a copy would give back a slot it never took.
         VirusScannerAutoCount(const VirusScannerAutoCount &);
         VirusScannerAutoCount & operator=(const VirusScannerAutoCount &);

         bool holds_slot_;
      };

      enum Constants
      {
         MaxRunningScanners = 10
      };


      static bool GetVirusScanningEnabled();

      // Returns true when a virus was found. scannerFailed is set when an enabled
      // scanner could not complete - it could not be launched, the socket to it
      // failed, it answered nothing usable - which is the difference between "this
      // message is clean" and "nobody looked". Callers that only care about the
      // verdict may pass nullptr, but the delivery path must not: for years a
      // scanner outage was indistinguishable from a clean result, so mail went out
      // unexamined and the only trace was one HM5406 in the error log.
      //
      // Deliberately NOT set for the two partial-scan cases the function reports
      // itself (HM6001, a message that could not be parsed for the per-attachment
      // pass, and HM6002, one attachment that could not be written out): the whole
      // message file was still scanned in both, and treating a transient file lock
      // as a scanner outage would hold ordinary mail. Nor for the max-size skip or
      // the scanner-slot give-up, which are policy and a wait respectively, not
      // failures.
      //
      // A message that is encrypted end to end is scanned as it stands - which
      // is to say its ciphertext is scanned - and a clean result from that is
      // not a verdict on the message. Scan says so in the application log, once
      // the whole file and every part have come back clean, and deliberately
      // does NOT set scannerFailed for it; see the comment at the point it is
      // written, and hmailserver/docs/SealedMail.md. A signed message carries
      // its text in the clear and a clean result on it is a verdict.
      static bool Scan(std::shared_ptr<Message> pMessage, String &virusName, bool *scannerFailed = nullptr);

      // Replaces the content of every attachment whose file name matches the
      // administrator's blocked-attachment list with a notice, and rewrites the
      // message. Both outcomes - a message rewritten, and a message left alone
      // because it is encrypted - are written to the application log, naming
      // the message. An encrypted message is left alone: its ciphertext is an
      // attachment by MIME type, so blocking would destroy it. A signed message
      // is checked like any other, because nothing verifies the signature and a
      // sender must not be able to opt out of the list with one header; the log
      // line for it says the signature has been invalidated.
      static void BlockAttachments(std::shared_ptr<Message> message);

      static void ResetCounter();

   private:

      // LONG, not long, because every access below goes through the interlocked
      // family and that family's argument is LONG. The two are the same type on
      // Windows; on a 64-bit POSIX build long is twice as wide, and a counter of
      // that width could not be handed to the atomic that guards it.
      static LONG running_scanners_;

      // Only VirusScannerAutoCount may take and give back slots, so that the
      // increment and the decrement can never get out of step again.
      static bool WaitForFreeScanner_();
      static void DecreaseCounter();
      static void ReportVirusFound(std::shared_ptr<Message> pMessage);
      // scannerError is set when any enabled scanner reported an error for this
      // file, whether or not a later scanner then answered.
      static VirusScanningResult ScanFile_(const String &fileName, bool &scannerError);
      static bool IsDetachedSignaturePart_(const AnsiString &contentType);

      static void ReportScanningError_(const VirusScanningResult &scanningResult);
   };

}
