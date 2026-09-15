// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// What a bounce means, in the four words a sender acts on: the mailbox does not
// exist, the mailbox is full, the receiving server refused on policy, or it was
// a temporary failure. Read off the RFC 3463 enhanced status code, and off the
// words of the reply only where the code cannot be trusted to say.
//
// The whole of this file is a function of values - no delivery, no database -
// so that the one decision that can put a live address on a suppression list is
// the one that is tested line by line (BounceClassifierTester, from ClassTester).
//
// THE TRUST RULE, and why it exists. DeliveryFailure::EnhancedStatusFromReplyCodeOnly
// turns a bare "550 <address>" at RCPT TO into 5.1.1, because that is what RFC 5321
// lets a 550 at that stage mean, and it is the right thing to put in a delivery
// status notification. It is the wrong thing to suppress an address on: a great
// many servers answer a policy refusal - a blocklisted sending address, a
// reputation score, a greylisting implementation in a bad mood - with a bare 550.
// So a code this server DERIVED is never believed as a dead mailbox. The remote's
// own enhanced code is recomputed from its verbatim reply, and only a code the
// remote itself sent is taken at its word.
//
// A failure this server gave itself - a connection that could not be made, a
// message the remote could not be sent in a form it accepts - carries no remote
// reply at all, and says nothing about the recipient's mailbox. It is never hard.

#pragma once

namespace HM
{
   enum BounceCategory
   {
      BounceNone = 0,         // delivered, or not a failure
      BounceHard = 1,         // the mailbox or its domain does not exist
      BounceSoft = 2,         // a temporary failure, which is every class 4 but X.2.2
      BounceMailboxFull = 3,  // X.2.2, in either class
      BouncePolicy = 4,       // X.7.x: refused on policy, which says nothing of the address
      BounceOther = 5         // a permanent failure that is none of the above
   };

   class BounceClassifier
   {
   public:

      struct Verdict
      {
         Verdict() : category(BounceNone), trusted(false) {}

         BounceCategory category;

         // True when the category rests on an enhanced code the remote itself
         // sent, or on a delivery report's own Status field.
         bool trusted;

         // The code the category was read from: the remote's own, or the one
         // this server recorded when the reply carried none.
         String status;
      };

      // A refusal met while this server was delivering. statusCode is what the
      // recipient recorded (MessageRecipient::GetEnhancedStatusCode); remoteReply
      // is the remote's words verbatim, three-digit code included, and empty when
      // no remote server answered.
      static Verdict ClassifyRefusal(const String &statusCode, const String &remoteReply);

      // A per-recipient group of an RFC 3464 report: its Status field and the
      // text of its Diagnostic-Code after the type ("smtp; ").
      static Verdict ClassifyReport(const String &status, const String &diagnostic);

      // The code table on its own: the category a code names, with nothing
      // about where it came from.
      static BounceCategory FromCode(const String &code);

      // The keyword table, applied to a reply whose code cannot be trusted.
      // classDigit is '4' or '5'. Returns BounceNone when no keyword matched.
      static BounceCategory FromKeywords(const String &text, wchar_t classDigit);

      // Whether a category counts towards the suppression threshold.
      static bool Suppresses(BounceCategory category, bool mailboxFullIsHard);

      // none, hard, soft, mailbox_full, policy, other.
      static const char *Name(BounceCategory category);
      static BounceCategory FromName(const String &name);

      // class "." subject "." detail, RFC 3463 2, class 2, 4 or 5.
      static bool IsEnhancedStatusCode(const String &token);

      // "5.1.1" out of "550 5.1.1 text", "550-5.1.1 text" or "5.1.1 text", and
      // only when the class digit agrees with a reply code that precedes it.
      // Empty when the text carries none.
      static String ExtractEnhancedCode(const String &text);

      // The three-digit reply code a text begins with, or 0.
      static int ReplyCode(const String &text);
   };

   class BounceClassifierTester
   {
   public:
      void Test();
   };
}
