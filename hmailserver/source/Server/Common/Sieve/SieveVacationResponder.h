// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <functional>
#include <vector>
#include <utility>

#include "SieveEvaluator.h"

namespace HM
{
   class Account;
   class MessageData;

   // Turns a SieveVacationDecision into an actual RFC 5230 auto-reply: applies the
   // per-sender suppression window, composes the response, and queues it.
   //
   // This is separate from SMTPVacationMessageCreator (the account-level "I am on
   // holiday" feature configured in hMailServer Administrator) rather than layered
   // on top of it, and that is worth explaining because the duplication is real.
   // The two differ in every part that matters here:
   //
   //   * that one tracks who it has replied to in a std::multimap that is empty
   //     again after a restart, and that has no expiry at all - the entry is removed
   //     only when the user switches the feature off. A ":days 7" window cannot be
   //     expressed with it, and a nightly service restart turns it into "reply to
   //     everyone, every day".
   //   * it keys on the sender alone, so RFC 5230's ":handle" - which is what lets
   //     one script hold two different vacation messages - has nowhere to go.
   //   * it takes the subject and body from the account's stored properties and
   //     expands hMailServer's own "%SUBJECT%" macro. A Sieve reason string is not
   //     macro-expanded (RFC 5230 has no macros, and a reason containing a literal
   //     percent sign must survive), and ":mime" means the reason may be a complete
   //     MIME entity rather than a body.
   //   * it sends with the account's own address as the envelope sender, where
   //     RFC 3834 4 requires a null return path for an automatic response.
   //   * it sends to whatever the caller passes, with the caller doing the
   //     loop-prevention. Here the loop prevention is the feature, so it lives with
   //     the sender.
   //
   // Changing SMTPVacationMessageCreator to cover both would mean changing the
   // behaviour of the existing account-level auto-reply, which is a separate,
   // documented, long-standing feature with its own COM properties and its own
   // Administrator user interface. That is not a change to make as a side effect of
   // adding Sieve vacation support.
   class SieveVacationResponder : public Singleton<SieveVacationResponder>
   {
   public:
      SieveVacationResponder();

      // Sends the auto-reply the decision describes, unless the suppression window
      // for this (sender, handle) is still running or a loop guard says no. Safe to
      // call with a decision whose "send" is false; it does nothing.
      //
      // Everything this needs about the message being replied to is in the decision,
      // so nothing here reads the message file. That matters: by the time an
      // auto-reply is queued the account-level message may already have been moved
      // into an IMAP folder, and a responder that went looking for it would work
      // only for the INBOX case.
      //
      // Never throws, and never reports a failure to the caller. The message the
      // script was filtering has already been accepted; failing its delivery because
      // an auto-reply could not be built would turn a cosmetic problem into lost
      // mail.
      void Respond(std::shared_ptr<const Account> account, const SieveVacationDecision &decision);

   private:
      // The window in seconds, from ":seconds" (RFC 6131) or ":days" (RFC 5230).
      static __int64 SuppressionWindowSeconds_(const SieveVacationDecision &decision);

      static String BuildSubject_(const SieveVacationDecision &decision);

      // Decodes a raw header value that may be RFC 2047 encoded, so that the
      // original subject can be quoted in the reply's own subject without being
      // double-encoded.
      static String DecodeHeaderValue_(const String &rawValue);

      // Splits a ":mime" reason into its MIME headers and its body.
      static void SplitMimeReason_(const String &reason,
                                   std::vector<std::pair<String, String>> &headers,
                                   String &body);

      // Applies a ":mime" reason to the reply being built.
      static void ApplyMimeReason_(MessageData &data, const String &reason);

      static bool ComposeAndSubmit_(std::shared_ptr<const Account> account,
                                    const SieveVacationDecision &decision,
                                    const String &fromAddress);
   };

   // The address a message a Sieve script makes may say it is from, for both actions
   // that take a ":from": vacation (RFC 5230 4.4) and notify (RFC 5435), whatever the
   // notification method.
   //
   // One rule for both: ":from" is used when it reaches the account - the account's
   // own address, in any case, or an active alias whose value is that address - and
   // otherwise the message is sent from the account's own address, with a line in the
   // application log. Anything else would let an account with a script send mail
   // through this server as any address at all. Vacation was held to this from the
   // start; a notification said it was from whatever ":from" said until 6 October 2026.
   //
   // What can be checked without the account - that ":from" is an address at all - the
   // validator checks when a script is stored, for both actions (SieveParser); this is
   // the half that needs the account, at delivery.
   class SieveFromAddress
   {
   public:
      enum class Action { Vacation, Notify };

      // ":from" if it reaches the account, the account's own address otherwise.
      static String Resolve(std::shared_ptr<const Account> account, const String &requested, Action action);

      // The same rule, with the account's address given and "does this other address
      // reach the account?" asked of reachesAccount rather than of the cache: Resolve
      // answers it with the account and alias lookups, SieveFromAddressTester with
      // examples. It is asked only of a ":from", trimmed, that is not the account's own
      // address. The action names the log line.
      static String Choose(const String &accountAddress, const String &requested, Action action,
                           const std::function<bool(const String &)> &reachesAccount);
   };

   // SieveFromAddress and the validator held to examples, for vacation and notify
   // alike: no ":from", the account's own address in another case and with blanks
   // around it, and an alias that reaches the account are used; another account's
   // address, the account's own followed by a line break and a field, and an address
   // with a display name are not, and come out as the account's own address. A notify
   // ":from" that is not an address is refused when the script is stored, as a vacation
   // ":from" is, a well-formed one is left to delivery, and a script stored before
   // holding a malformed one still runs, with the account's own address as the
   // notification's sender. No configuration, database or network. ClassTester runs it
   // on Windows, business-object-tests on Linux. Throws std::logic_error naming every
   // case that went wrong.
   class SieveFromAddressTester
   {
   public:
      void Test();
   };
}
