// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// RFC 6238 time-based one-time passwords, in the SERVER.
//
// TOTP already existed in this product and was invisible from here: the Control
// Panel implements it in C# and checks the code itself, after the server has
// already accepted the administrator's password. That is a client-side check of
// a client-side secret, which is fine for what it does - locking the admin tool -
// and is why "TOTP is supported" has never meant a mailbox account could have a
// second factor. The server had no idea the feature existed.
//
// Deliberately the same parameters as the Control Panel's implementation
// (HMAC-SHA1, 30-second step, 6 digits, one step of tolerance either side), so a
// secret enrolled in one is verifiable by the other and an authenticator app
// behaves identically against both. RFC 6238 permits SHA-256 and SHA-512, and
// essentially no authenticator app implements them; using one here would produce
// a QR code that scans and then never matches.
//
// The +/-1 step tolerance is the interoperability floor rather than a
// preference: without it, a client whose clock is a second out fails at the
// boundary of every window, and the failure is intermittent in the way most
// likely to be blamed on the server. It costs an attacker nothing meaningful -
// three codes out of a million rather than one.
//
// Nothing here reads configuration or touches a database: the secret and the
// time both arrive as parameters. That is what lets the whole of it be pinned by
// tests against RFC 6238's published vectors, which is the only way to be sure a
// TOTP implementation is right - "it works with my phone" tests the phone.

#pragma once

namespace HM
{
   class Totp
   {
   public:
      // Verifies a 6-digit code against a base32 secret at the given Unix time,
      // accepting the adjacent steps either side. False for an empty secret, an
      // empty or malformed code, or a secret that is not valid base32 - never an
      // exception, because this sits on an authentication path.
      static bool VerifyCode(const AnsiString &base32Secret, const AnsiString &code, time_t now);

      // As above, at the current time.
      static bool VerifyCode(const AnsiString &base32Secret, const AnsiString &code);

      // As above, and says WHICH 30-second step the code matched: the latest,
      // should the same six digits come up at two steps of the window.
      // SecondFactorGuard needs it to refuse a code used once already.
      static bool VerifyCode(const AnsiString &base32Secret, const AnsiString &code, time_t now, __int64 &matchedStep);

      // The code for one 30-second counter value. Public so the RFC 6238 test
      // vectors can be pinned directly rather than inferred through VerifyCode.
      static AnsiString ComputeCode(const AnsiString &base32Secret, __int64 counter);

      // A fresh 160-bit secret, base32 encoded - the size RFC 4226 recommends and
      // what every authenticator app expects. Empty if the CSPRNG fails, which the
      // caller must treat as a failure to enrol rather than as an empty secret,
      // since an empty secret means "no second factor" everywhere else here.
      static AnsiString GenerateSecret();

      // The otpauth:// URI an authenticator app scans. The label is the account
      // address; the issuer is fixed, so all of this server's accounts group
      // together in the app rather than appearing as unrelated entries.
      static AnsiString BuildOtpAuthUri(const AnsiString &accountAddress, const AnsiString &base32Secret);

      // True when the string is syntactically usable as a secret. Used when a
      // secret arrives from outside rather than from GenerateSecret.
      static bool IsValidSecret(const AnsiString &base32Secret);

   private:
      static bool Base32Decode_(const AnsiString &input, std::vector<unsigned char> &output);
      static AnsiString Base32Encode_(const std::vector<unsigned char> &input);
   };

   // What every place that checks a one-time code holds in common beyond the
   // arithmetic above - the COM sign-in (COMAuthentication), the REST API's
   // administrator Basic credential and the REST routes that ask for a code on
   // top of a password (SecondFactorProved_, ConfirmAdministratorPassword_) -
   // so that a code, and a guess at one, counts the same whichever of them it
   // was offered to. An owner is an account's address, or "administrator".
   //
   // Two things are remembered for each owner, in memory, as the per-name
   // lockout's counts are:
   //
   // - The step of the last code taken. RFC 6238 section 5.2: a verifier MUST
   //   NOT accept a code a second time once it has accepted it. Without this a
   //   code somebody read over a shoulder, out of a proxy's log or from a
   //   phishing page stayed good for the ninety seconds of its window, and the
   //   password beside it made it a whole credential. A code is taken only
   //   when its step is later than the last one taken for the owner's secret,
   //   so the code before it in the window is refused too. The secret is
   //   recognised by a hash of it, so a secret enrolled afresh starts clean.
   //
   // - The wrong codes: five in fifteen minutes, and then fifteen minutes in
   //   which no code is taken, the right one included - a lock the right answer
   //   opened would stop no guessing at all. A code used once already is
   //   refused but NOT counted: it is no guess - it can never be taken, whoever
   //   sends it - and the person most likely to send it is the owner, again,
   //   within the half minute the authenticator still shows it (a form sent a
   //   second time after a refusal for another reason, a script that sends a
   //   code with every request). Counting it would lock the owner out, and on
   //   the REST API charge their address to the auto-ban, for that.
   //
   //   The lock is looked at, the code judged and a wrong one counted under
   //   one lock (TakeCode), so that requests racing each other cannot all be
   //   judged before any of them is counted.
   //
   // A restart of the service clears both, and a Reinitialize, or a stop of the
   // REST listener, clears the wrong-code counts (ClearBudgets). The steps are
   // kept through those: forgetting them would let a code be used again, and
   // they are forgotten by themselves once the step has left every window.
   class SecondFactorGuard
   {
   public:
      enum Taken
      {
         CodeTaken,     // Verified, and its step not used before: remembered now.
         CodeWrong,     // Not a code of this secret in the window: counted.
         CodeReused,    // A code of this secret whose step has been taken already: not counted.
         CodeLocked     // The owner's wrong-code budget is spent: not looked at.
      };

      // What TakeCode found beside its answer.
      struct Outcome
      {
         int wrong = 0;            // CodeWrong: wrong codes in the last fifteen minutes, this one included.
         bool locked_now = false;  // CodeWrong: this one spent the budget.
         int retry_after = 0;      // CodeLocked: the seconds the lock has left.
      };

      // Under one lock: while the owner's budget is spent the code is not
      // looked at (CodeLocked); a good new code is taken - its step remembered
      // as the owner's last and the wrong-code count forgotten (CodeTaken); a
      // code whose step has been taken is refused and not counted
      // (CodeReused); and any other code is counted as wrong (CodeWrong). The
      // caller logs, and decides what else a wrong code costs - the auto-ban,
      // the per-name lockout - from the outcome.
      static Taken TakeCode(const String &owner, const AnsiString &base32Secret, const AnsiString &code, Outcome *outcome = nullptr);
      static Taken TakeCode(const String &owner, const AnsiString &base32Secret, const AnsiString &code, time_t now, Outcome *outcome = nullptr);

      // True, with the seconds left, while the owner's wrong-code budget is
      // spent: for a caller with something other than a one-time code to judge
      // (a recovery code), which asks before it looks.
      static bool Locked(const String &owner, int &retryAfter);
      static bool Locked(const String &owner, int &retryAfter, time_t now);

      // One wrong code for the owner that TakeCode did not judge - a recovery
      // code, say. Answers how many there have been in the last fifteen
      // minutes; lockedNow when this one spent the budget.
      static int CountWrong(const String &owner, bool &lockedNow);
      static int CountWrong(const String &owner, bool &lockedNow, time_t now);

      // A right code: the owner's wrong-code count is forgotten.
      static void CountRight(const String &owner);

      // A second factor enrolled or removed: everything about the owner goes,
      // the count, the lock and the step, since they were about another secret.
      static void ForgetOwner(const String &owner);

      // Every owner's wrong-code count and lock, not the steps taken.
      static void ClearBudgets();

      static const int WrongCodesAllowed = 5;
      static const int WrongCodeWindowSeconds = 15 * 60;
      static const int WrongCodeLockSeconds = 15 * 60;
   };

   // Pins RFC 6238's published test vectors. This is the only way to know a TOTP
   // implementation is correct rather than merely self-consistent: an
   // implementation that agrees with itself will happily agree with itself about
   // the wrong answer, and the symptom - "my authenticator app's codes are never
   // accepted" - looks like a clock problem, a QR problem or a typing problem long
   // before anyone suspects the truncation.
   class TotpTester
   {
   public:
      void Test();

   private:
      void TestSecondFactorGuard_(const AnsiString &secret);
   };
}
