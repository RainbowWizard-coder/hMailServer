// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "Totp.h"

#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const int PeriodSeconds = 30;
      const int Digits = 6;
      const char *Base32Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
      const int SecretBytes = 20;   // 160 bits, per RFC 4226 section 4
   }

   bool
   Totp::Base32Decode_(const AnsiString &input, std::vector<unsigned char> &output)
   {
      output.clear();

      int buffer = 0;
      int bitsLeft = 0;

      for (int i = 0; i < input.GetLength(); i++)
      {
         char c = input.GetAt(i);

         // Padding and separators are tolerated: authenticator apps and the people
         // who retype secrets both add them, and a secret that differs only by a
         // space is not a different secret.
         if (c == '=' || c == ' ' || c == '-')
            continue;

         if (c >= 'a' && c <= 'z')
            c = (char) (c - 'a' + 'A');

         const char *found = strchr(Base32Alphabet, c);

         if (found == nullptr || c == 0)
            return false;

         buffer = (buffer << 5) | (int) (found - Base32Alphabet);
         bitsLeft += 5;

         if (bitsLeft >= 8)
         {
            bitsLeft -= 8;
            output.push_back((unsigned char) ((buffer >> bitsLeft) & 0xFF));
         }
      }

      return !output.empty();
   }

   AnsiString
   Totp::Base32Encode_(const std::vector<unsigned char> &input)
   {
      AnsiString result;

      int buffer = 0;
      int bitsLeft = 0;

      for (size_t i = 0; i < input.size(); i++)
      {
         buffer = (buffer << 8) | input[i];
         bitsLeft += 8;

         while (bitsLeft >= 5)
         {
            bitsLeft -= 5;
            char c = Base32Alphabet[(buffer >> bitsLeft) & 0x1F];
            result.append(&c, 1);
         }
      }

      if (bitsLeft > 0)
      {
         char c = Base32Alphabet[(buffer << (5 - bitsLeft)) & 0x1F];
         result.append(&c, 1);
      }

      return result;
   }

   bool
   Totp::IsValidSecret(const AnsiString &base32Secret)
   {
      std::vector<unsigned char> key;
      return Base32Decode_(base32Secret, key);
   }

   AnsiString
   Totp::ComputeCode(const AnsiString &base32Secret, __int64 counter)
   {
      std::vector<unsigned char> key;

      if (!Base32Decode_(base32Secret, key))
         return "";

      // RFC 4226 section 5.1: the counter is eight bytes, big-endian, regardless of
      // the host's byte order.
      unsigned char counterBytes[8];
      for (int i = 7; i >= 0; i--)
      {
         counterBytes[i] = (unsigned char) (counter & 0xFF);
         counter >>= 8;
      }

      unsigned char digest[EVP_MAX_MD_SIZE];
      unsigned int digestLength = 0;

      if (HMAC(EVP_sha1(), &key[0], (int) key.size(), counterBytes, sizeof(counterBytes),
               digest, &digestLength) == nullptr || digestLength < 20)
      {
         return "";
      }

      // RFC 4226 section 5.3, dynamic truncation. The masks are not decoration: the
      // high bit of the four selected bytes is cleared so the result is the same on
      // a machine that treats the value as signed.
      int offset = digest[digestLength - 1] & 0x0F;

      int binary =
         ((digest[offset] & 0x7F) << 24) |
         ((digest[offset + 1] & 0xFF) << 16) |
         ((digest[offset + 2] & 0xFF) << 8) |
         (digest[offset + 3] & 0xFF);

      int modulus = 1;
      for (int i = 0; i < Digits; i++)
         modulus *= 10;

      AnsiString result;
      result.Format("%0*d", Digits, binary % modulus);

      return result;
   }

   bool
   Totp::VerifyCode(const AnsiString &base32Secret, const AnsiString &code, time_t now)
   {
      __int64 matchedStep = 0;
      return VerifyCode(base32Secret, code, now, matchedStep);
   }

   bool
   Totp::VerifyCode(const AnsiString &base32Secret, const AnsiString &code, time_t now, __int64 &matchedStep)
   {
      matchedStep = 0;

      if (base32Secret.IsEmpty() || code.IsEmpty())
         return false;

      AnsiString trimmed = code;
      trimmed.Trim();

      if (trimmed.GetLength() != Digits)
         return false;

      for (int i = 0; i < trimmed.GetLength(); i++)
      {
         if (!isdigit((unsigned char) trimmed.GetAt(i)))
            return false;
      }

      __int64 counter = (__int64) now / PeriodSeconds;

      // Compared over the whole accepted window rather than returning on the first
      // match, so the time taken does not reveal WHICH step matched. That leaks
      // nothing about the secret, but it does leak the client's clock offset, and a
      // comparison on an authentication path is the wrong place to be casual.
      bool matched = false;

      for (__int64 offset = -1; offset <= 1; offset++)
      {
         AnsiString expected = ComputeCode(base32Secret, counter + offset);

         if (expected.IsEmpty())
            return false;

         // Fixed-time comparison of two equal-length digit strings.
         unsigned char difference = 0;
         for (int i = 0; i < Digits; i++)
            difference |= (unsigned char) (expected.GetAt(i) ^ trimmed.GetAt(i));

         // The latest step that matches, the loop running from the earliest:
         // the same six digits at two steps of one window is a chance of three
         // in a million, and taking the later step then refuses more, not less.
         if (difference == 0)
         {
            matched = true;
            matchedStep = counter + offset;
         }
      }

      return matched;
   }

   bool
   Totp::VerifyCode(const AnsiString &base32Secret, const AnsiString &code)
   {
      return VerifyCode(base32Secret, code, time(nullptr));
   }

   namespace
   {
      // SecondFactorGuard's memory, one entry for each owner that has had a
      // code taken in the last window or a wrong code in the last fifteen
      // minutes. The ceiling only decides when idle entries are swept out.
      const size_t GuardOwnersCeiling = 10000;

      struct GuardOwner
      {
         std::deque<time_t> wrong_at;
         time_t locked_until = 0;
         bool has_step = false;
         __int64 last_step = 0;
         size_t secret_tag = 0;
      };

      std::mutex guard_mutex;
      std::map<String, GuardOwner> guard_owners;

      String GuardKey(const String &owner)
      {
         String key = owner;
         key.Trim();
         key.ToLower();
         return key;
      }

      size_t SecretTag(const AnsiString &base32Secret)
      {
         AnsiString normalised = base32Secret;
         normalised.Trim();
         normalised.ToUpper();
         return std::hash<std::string>()(static_cast<const std::string &>(normalised));
      }

      // Called with guard_mutex held.
      void DropOldWrongCodes(GuardOwner &state, time_t now)
      {
         while (!state.wrong_at.empty() && now - state.wrong_at.front() >= SecondFactorGuard::WrongCodeWindowSeconds)
            state.wrong_at.pop_front();
      }

      // Called with guard_mutex held. An entry is idle when it counts nothing
      // and its step has left every window: a step two behind the current one
      // is refused by the window itself, so remembering it refuses nothing.
      bool Idle(GuardOwner &state, time_t now)
      {
         DropOldWrongCodes(state, now);
         const __int64 currentStep = (__int64) now / PeriodSeconds;
         return state.wrong_at.empty() && state.locked_until <= now &&
                (!state.has_step || state.last_step < currentStep - 1);
      }

      void SweepIdle(time_t now)
      {
         for (auto it = guard_owners.begin(); it != guard_owners.end();)
         {
            if (Idle(it->second, now))
               it = guard_owners.erase(it);
            else
               ++it;
         }
      }

      GuardOwner &Owner(const String &owner, time_t now)
      {
         if (guard_owners.size() >= GuardOwnersCeiling)
            SweepIdle(now);
         return guard_owners[GuardKey(owner)];
      }

      // Called with guard_mutex held: one wrong code counted, and the budget
      // spent by the fifth in the window.
      int CountWrongHeld(GuardOwner &state, time_t now, bool &lockedNow)
      {
         lockedNow = false;

         DropOldWrongCodes(state, now);
         state.wrong_at.push_back(now);
         const int wrong = (int) state.wrong_at.size();

         if (wrong >= SecondFactorGuard::WrongCodesAllowed && state.locked_until <= now)
         {
            state.locked_until = now + SecondFactorGuard::WrongCodeLockSeconds;
            state.wrong_at.clear();
            lockedNow = true;
         }

         return wrong;
      }
   }

   SecondFactorGuard::Taken
   SecondFactorGuard::TakeCode(const String &owner, const AnsiString &base32Secret, const AnsiString &code, Outcome *outcome)
   {
      return TakeCode(owner, base32Secret, code, time(nullptr), outcome);
   }

   SecondFactorGuard::Taken
   SecondFactorGuard::TakeCode(const String &owner, const AnsiString &base32Secret, const AnsiString &code, time_t now, Outcome *outcome)
   {
      Outcome ignored;
      Outcome &result = outcome ? *outcome : ignored;
      result = Outcome();

      const size_t tag = SecretTag(base32Secret);

      // The lock looked at, the code judged, and a wrong one counted or a good
      // one remembered, all under one lock: two requests carrying the same code
      // at the same moment cannot both be the first, and requests racing each
      // other with guesses cannot all be judged before the fifth is counted.
      std::lock_guard<std::mutex> guard(guard_mutex);
      GuardOwner &state = Owner(owner, now);

      if (state.locked_until > now)
      {
         result.retry_after = (int) (state.locked_until - now);
         if (result.retry_after < 1)
            result.retry_after = 1;
         return CodeLocked;
      }

      __int64 step = 0;
      if (!Totp::VerifyCode(base32Secret, code, now, step))
      {
         result.wrong = CountWrongHeld(state, now, result.locked_now);
         return CodeWrong;
      }

      // Refused and not counted: a code lands here only by being one that
      // was right once, which can never be taken again (the header, Totp.h).
      if (state.has_step && state.secret_tag == tag && step <= state.last_step)
         return CodeReused;

      state.has_step = true;
      state.secret_tag = tag;
      state.last_step = step;
      state.wrong_at.clear();
      return CodeTaken;
   }

   bool
   SecondFactorGuard::Locked(const String &owner, int &retryAfter)
   {
      return Locked(owner, retryAfter, time(nullptr));
   }

   bool
   SecondFactorGuard::Locked(const String &owner, int &retryAfter, time_t now)
   {
      retryAfter = 0;

      std::lock_guard<std::mutex> guard(guard_mutex);
      auto found = guard_owners.find(GuardKey(owner));
      if (found == guard_owners.end() || found->second.locked_until <= now)
         return false;

      retryAfter = (int) (found->second.locked_until - now);
      if (retryAfter < 1)
         retryAfter = 1;
      return true;
   }

   int
   SecondFactorGuard::CountWrong(const String &owner, bool &lockedNow)
   {
      return CountWrong(owner, lockedNow, time(nullptr));
   }

   int
   SecondFactorGuard::CountWrong(const String &owner, bool &lockedNow, time_t now)
   {
      std::lock_guard<std::mutex> guard(guard_mutex);
      return CountWrongHeld(Owner(owner, now), now, lockedNow);
   }

   void
   SecondFactorGuard::CountRight(const String &owner)
   {
      std::lock_guard<std::mutex> guard(guard_mutex);
      auto found = guard_owners.find(GuardKey(owner));
      if (found == guard_owners.end())
         return;

      // The count only: a right code is looked at only while there is no lock.
      found->second.wrong_at.clear();
   }

   void
   SecondFactorGuard::ForgetOwner(const String &owner)
   {
      std::lock_guard<std::mutex> guard(guard_mutex);
      guard_owners.erase(GuardKey(owner));
   }

   void
   SecondFactorGuard::ClearBudgets()
   {
      std::lock_guard<std::mutex> guard(guard_mutex);

      for (auto &entry : guard_owners)
      {
         entry.second.wrong_at.clear();
         entry.second.locked_until = 0;
      }

      SweepIdle(time(nullptr));
   }

   AnsiString
   Totp::GenerateSecret()
   {
      std::vector<unsigned char> buffer(SecretBytes);

      if (RAND_bytes(&buffer[0], SecretBytes) != 1)
      {
         // Empty means "no second factor" everywhere else here, so the caller has to
         // treat this as a failure to enrol rather than storing it.
         ErrorManager::Instance()->ReportError(ErrorManager::High, 6199, "Totp::GenerateSecret",
            "The random number generator failed, so no TOTP secret was generated. Nothing has been stored.");

         return "";
      }

      return Base32Encode_(buffer);
   }

   AnsiString
   Totp::BuildOtpAuthUri(const AnsiString &accountAddress, const AnsiString &base32Secret)
   {
      AnsiString uri;

      // The label is percent-encoded because an address may contain characters the
      // URI grammar treats as delimiters, and an authenticator app given a broken
      // URI reports nothing more useful than "invalid QR code".
      AnsiString encodedLabel;

      for (int i = 0; i < accountAddress.GetLength(); i++)
      {
         char c = accountAddress.GetAt(i);

         if (isalnum((unsigned char) c) || c == '-' || c == '.' || c == '_' || c == '~')
         {
            encodedLabel.append(&c, 1);
         }
         else
         {
            AnsiString escaped;
            escaped.Format("%%%02X", (unsigned char) c);
            encodedLabel += escaped;
         }
      }

      uri.Format("otpauth://totp/%s?secret=%s&issuer=hMailServer&digits=%d&period=%d",
                 encodedLabel.c_str(), base32Secret.c_str(), Digits, PeriodSeconds);

      return uri;
   }

   void
   TotpTester::Test()
   {
      // RFC 6238 Appendix B, the SHA-1 rows. The published secret is the ASCII
      // string "12345678901234567890"; base32 that is the value below, which is
      // what an authenticator app would be given.
      //
      // The RFC prints eight-digit codes and this server issues six, which is not a
      // discrepancy: RFC 4226 section 5.3 truncates with "modulo 10^Digits", so the
      // six-digit code is the last six digits of the eight-digit one. Getting that
      // relationship wrong is the classic TOTP bug and it is invisible without
      // vectors.
      const AnsiString secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";

      struct Vector { __int64 unixTime; const char *code; };

      const Vector vectors[] =
      {
         {          59LL, "287082" },   // 94287082
         {  1111111109LL, "081804" },   // 07081804
         {  1111111111LL, "050471" },   // 14050471
         {  1234567890LL, "005924" },   // 89005924
         {  2000000000LL, "279037" },   // 69279037
         { 20000000000LL, "353130" },   // 65353130 - past 2038, so this also pins
                                        // that the counter is 64-bit throughout
      };

      for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++)
      {
         AnsiString actual = Totp::ComputeCode(secret, vectors[i].unixTime / 30);

         if (actual.Compare(vectors[i].code) != 0)
            throw 0;

         // ...and the verifier accepts it at that time, which is the thing callers
         // actually use.
         if (!Totp::VerifyCode(secret, vectors[i].code, (time_t) vectors[i].unixTime))
            throw 0;
      }

      // One step either side is accepted, and two is not. Without the tolerance a
      // client whose clock is a second out fails at the boundary of every window;
      // with too much of it, a code stays valid long after it has been shoulder-surfed.
      if (!Totp::VerifyCode(secret, "050471", (time_t) (1111111111LL + 30)))
         throw 0;

      if (!Totp::VerifyCode(secret, "050471", (time_t) (1111111111LL - 30)))
         throw 0;

      if (Totp::VerifyCode(secret, "050471", (time_t) (1111111111LL + 90)))
         throw 0;

      // Malformed input is refused rather than throwing, because this runs on an
      // authentication path.
      if (Totp::VerifyCode(secret, "", (time_t) 1111111111LL)) throw 0;
      if (Totp::VerifyCode("", "050471", (time_t) 1111111111LL)) throw 0;
      if (Totp::VerifyCode(secret, "05047", (time_t) 1111111111LL)) throw 0;      // too short
      if (Totp::VerifyCode(secret, "0504711", (time_t) 1111111111LL)) throw 0;    // too long
      if (Totp::VerifyCode(secret, "05047a", (time_t) 1111111111LL)) throw 0;     // not digits
      if (Totp::VerifyCode("not!base32", "050471", (time_t) 1111111111LL)) throw 0;

      // A generated secret is usable and round-trips through the decoder.
      AnsiString generated = Totp::GenerateSecret();

      if (generated.GetLength() != 32 || !Totp::IsValidSecret(generated))
         throw 0;

      if (Totp::GenerateSecret().Compare(generated) == 0)
         throw 0;   // two secrets in a row must differ

      // The otpauth URI must percent-encode the address, or an app given an
      // address containing '@' reports nothing more useful than "invalid QR code".
      AnsiString uri = Totp::BuildOtpAuthUri("user@example.com", secret);

      if (uri.Find("user%40example.com") < 0 || uri.Find(secret) < 0)
         throw 0;

      TestSecondFactorGuard_(secret);
   }

   void
   TotpTester::TestSecondFactorGuard_(const AnsiString &secret)
   {
      // Owners no real account can have (.invalid), at a time long past, so
      // that nothing a real owner has is touched and the steps are fixed. At
      // 1111111111 the step is 37037037 and the RFC secret's codes are 081804
      // (the step before), 050471 (this one), 266759 (the next) and 511787
      // (five on).
      const String owner = _T("totp-tester@guard.invalid");
      const String otherOwner = _T("totp-tester-other@guard.invalid");
      const time_t at = (time_t) 1111111111LL;

      SecondFactorGuard::ForgetOwner(owner);
      SecondFactorGuard::ForgetOwner(otherOwner);

      // Taken once, and then never again while it is in the window
      // (RFC 6238 section 5.2) - the step after included.
      if (SecondFactorGuard::TakeCode(owner, secret, "050471", at) != SecondFactorGuard::CodeTaken) throw 0;
      if (SecondFactorGuard::TakeCode(owner, secret, "050471", at) != SecondFactorGuard::CodeReused) throw 0;
      if (SecondFactorGuard::TakeCode(owner, secret, " 050471 ", at + 30) != SecondFactorGuard::CodeReused) throw 0;

      // The code before it in the window is refused as well: taking a code
      // puts every earlier step behind the owner.
      if (SecondFactorGuard::TakeCode(owner, secret, "081804", at) != SecondFactorGuard::CodeReused) throw 0;

      // The next step's code is new, once.
      if (SecondFactorGuard::TakeCode(owner, secret, "266759", at) != SecondFactorGuard::CodeTaken) throw 0;
      if (SecondFactorGuard::TakeCode(owner, secret, "266759", at + 30) != SecondFactorGuard::CodeReused) throw 0;

      // Not a code of the window at all, or not a code: wrong, not reused.
      if (SecondFactorGuard::TakeCode(owner, secret, "511787", at) != SecondFactorGuard::CodeWrong) throw 0;
      if (SecondFactorGuard::TakeCode(owner, secret, "05047a", at) != SecondFactorGuard::CodeWrong) throw 0;

      // Each owner's steps are its own, and so are its secret's: a secret
      // enrolled afresh starts clean.
      if (SecondFactorGuard::TakeCode(otherOwner, secret, "050471", at) != SecondFactorGuard::CodeTaken) throw 0;
      if (SecondFactorGuard::TakeCode(owner, "JBSWY3DPEHPK3PXP", "358462", at) != SecondFactorGuard::CodeTaken) throw 0;
      if (SecondFactorGuard::TakeCode(owner, "JBSWY3DPEHPK3PXP", "358462", at) != SecondFactorGuard::CodeReused) throw 0;

      // And forgetting an owner (a factor enrolled or removed) forgets its steps.
      SecondFactorGuard::ForgetOwner(otherOwner);
      if (SecondFactorGuard::TakeCode(otherOwner, secret, "050471", at) != SecondFactorGuard::CodeTaken) throw 0;

      // The wrong-code budget: four are counted and change nothing...
      int retryAfter = 0;
      bool lockedNow = false;
      for (int i = 1; i <= 4; i++)
      {
         if (SecondFactorGuard::CountWrong(owner, lockedNow, at) != i || lockedNow) throw 0;
         if (SecondFactorGuard::Locked(owner, retryAfter, at)) throw 0;
      }

      // ...the fifth spends it, for fifteen minutes and no longer...
      SecondFactorGuard::CountWrong(owner, lockedNow, at);
      if (!lockedNow) throw 0;
      if (!SecondFactorGuard::Locked(owner, retryAfter, at + 1) || retryAfter != SecondFactorGuard::WrongCodeLockSeconds - 1) throw 0;
      if (SecondFactorGuard::Locked(owner, retryAfter, at + SecondFactorGuard::WrongCodeLockSeconds)) throw 0;

      // ...another owner's budget is untouched by it...
      if (SecondFactorGuard::Locked(otherOwner, retryAfter, at + 1)) throw 0;

      // ...a right code forgets the count...
      const time_t later = at + SecondFactorGuard::WrongCodeLockSeconds;
      SecondFactorGuard::CountWrong(owner, lockedNow, later);
      SecondFactorGuard::CountWrong(owner, lockedNow, later);
      SecondFactorGuard::CountRight(owner);
      if (SecondFactorGuard::CountWrong(owner, lockedNow, later) != 1) throw 0;

      // ...and a wrong code older than fifteen minutes has stopped counting.
      if (SecondFactorGuard::CountWrong(owner, lockedNow, later + SecondFactorGuard::WrongCodeWindowSeconds) != 1) throw 0;

      // TakeCode counts a wrong code itself, under the lock it judges it in:
      // four change nothing, the fifth spends the budget, and then the right
      // code is not looked at (CodeLocked, with the time left).
      const String guessed = _T("totp-tester-guessed@guard.invalid");
      SecondFactorGuard::ForgetOwner(guessed);
      SecondFactorGuard::Outcome outcome;
      for (int i = 1; i <= 4; i++)
      {
         if (SecondFactorGuard::TakeCode(guessed, secret, "511787", at, &outcome) != SecondFactorGuard::CodeWrong) throw 0;
         if (outcome.wrong != i || outcome.locked_now) throw 0;
      }
      if (SecondFactorGuard::TakeCode(guessed, secret, "511787", at, &outcome) != SecondFactorGuard::CodeWrong || !outcome.locked_now) throw 0;
      if (SecondFactorGuard::TakeCode(guessed, secret, "050471", at + 1, &outcome) != SecondFactorGuard::CodeLocked) throw 0;
      if (outcome.retry_after != SecondFactorGuard::WrongCodeLockSeconds - 1) throw 0;

      // A code taken forgets the wrong ones before it.
      SecondFactorGuard::ForgetOwner(guessed);
      SecondFactorGuard::TakeCode(guessed, secret, "511787", at, &outcome);
      SecondFactorGuard::TakeCode(guessed, secret, "511787", at, &outcome);
      if (SecondFactorGuard::TakeCode(guessed, secret, "050471", at, &outcome) != SecondFactorGuard::CodeTaken) throw 0;
      if (SecondFactorGuard::TakeCode(guessed, secret, "511787", at, &outcome) != SecondFactorGuard::CodeWrong || outcome.wrong != 1) throw 0;

      // A code used once already is refused and NOT counted: it is no guess,
      // and the one who sends it again is most likely its owner. Ten of them
      // leave the budget as it was, and the next step's code is taken.
      SecondFactorGuard::ForgetOwner(guessed);
      if (SecondFactorGuard::TakeCode(guessed, secret, "050471", at) != SecondFactorGuard::CodeTaken) throw 0;
      for (int i = 0; i < 10; i++)
      {
         if (SecondFactorGuard::TakeCode(guessed, secret, "050471", at, &outcome) != SecondFactorGuard::CodeReused) throw 0;
         if (outcome.wrong != 0 || outcome.locked_now) throw 0;
      }
      if (SecondFactorGuard::Locked(guessed, retryAfter, at)) throw 0;
      if (SecondFactorGuard::TakeCode(guessed, secret, "266759", at) != SecondFactorGuard::CodeTaken) throw 0;

      SecondFactorGuard::ForgetOwner(guessed);
      SecondFactorGuard::ForgetOwner(owner);
      SecondFactorGuard::ForgetOwner(otherOwner);
   }
}
