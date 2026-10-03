// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "SignInState.h"

#include "MailboxCrypto.h"
#include "WebAuthn.h"

#include <map>
#include <mutex>
#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // No state is longer than this: an OIDC state of four base64url values and
      // a time is under three hundred characters sealed. Anything longer is not
      // one, and is not decoded.
      const int MaxSealedLength = 1024;

      std::mutex state_mutex;
      std::string state_key;

      // Spent states by kind, each id with the time it stops mattering.
      std::map<std::string, std::map<std::string, time_t>> spent;

      std::string KeyLocked()
      {
         if (state_key.size() != (size_t) MailboxCrypto::KeyBytes)
            state_key = MailboxCrypto::RandomBytes((size_t) MailboxCrypto::KeyBytes);
         return state_key;
      }

      std::string AssociatedData(const char *kind)
      {
         return std::string("hMailServer sign-in state 1\n") + kind;
      }

      void PruneLocked(std::map<std::string, time_t> &set, time_t now)
      {
         for (auto it = set.begin(); it != set.end();)
         {
            if (it->second < now)
               it = set.erase(it);
            else
               ++it;
         }
      }
   }

   AnsiString
   SignInState::Seal(const char *kind, const std::string &plain)
   {
      std::string key;
      {
         std::lock_guard<std::mutex> guard(state_mutex);
         key = KeyLocked();
      }
      MailboxCrypto::ForgetOnExit forgetKey(key);

      if (key.size() != (size_t) MailboxCrypto::KeyBytes)
         return "";

      const std::string nonce = MailboxCrypto::RandomBytes((size_t) MailboxCrypto::NonceBytes);
      if (nonce.size() != (size_t) MailboxCrypto::NonceBytes)
         return "";

      std::string cipher;
      std::string tag;
      if (!MailboxCrypto::Encrypt(key, nonce, AssociatedData(kind), plain, cipher, tag))
         return "";

      return WebAuthn::Base64Url(nonce + cipher + tag);
   }

   bool
   SignInState::Open(const char *kind, const AnsiString &sealed, std::string &plain)
   {
      plain.clear();

      if (sealed.IsEmpty() || sealed.GetLength() > MaxSealedLength)
         return false;

      std::string bytes;
      if (!WebAuthn::Base64UrlDecode(std::string(sealed.c_str(), (size_t) sealed.GetLength()), bytes) ||
          bytes.size() < (size_t) MailboxCrypto::NonceBytes + (size_t) MailboxCrypto::TagBytes)
         return false;

      const size_t nonceBytes = (size_t) MailboxCrypto::NonceBytes;
      const size_t tagBytes = (size_t) MailboxCrypto::TagBytes;
      const std::string nonce = bytes.substr(0, nonceBytes);
      const std::string tag = bytes.substr(bytes.size() - tagBytes);
      const std::string cipher = bytes.substr(nonceBytes, bytes.size() - nonceBytes - tagBytes);

      std::string key;
      {
         std::lock_guard<std::mutex> guard(state_mutex);
         key = KeyLocked();
      }
      MailboxCrypto::ForgetOnExit forgetKey(key);

      return MailboxCrypto::Decrypt(key, nonce, AssociatedData(kind), cipher, tag, plain);
   }

   AnsiString
   SignInState::Tag(const char *kind, const std::string &data)
   {
      std::string key;
      {
         std::lock_guard<std::mutex> guard(state_mutex);
         key = KeyLocked();
      }
      MailboxCrypto::ForgetOnExit forgetKey(key);

      const std::string tag = MailboxCrypto::Hkdf(key, AssociatedData(kind), data, 16);
      if (tag.size() != 16)
         return "";

      return WebAuthn::Base64Url(tag);
   }

   bool
   SignInState::Verify(const char *kind, const std::string &data, const AnsiString &tag)
   {
      const AnsiString expected = Tag(kind, data);
      if (expected.IsEmpty() || tag.IsEmpty())
         return false;

      return MailboxCrypto::ConstantTimeEquals(std::string(expected.c_str(), (size_t) expected.GetLength()),
                                               std::string(tag.c_str(), (size_t) tag.GetLength()));
   }

   bool
   SignInState::Spend(const char *kind, const std::string &id, time_t expires)
   {
      if (id.empty())
         return false;

      const time_t now = time(nullptr);
      std::lock_guard<std::mutex> guard(state_mutex);
      std::map<std::string, time_t> &set = spent[kind];
      PruneLocked(set, now);

      if (set.find(id) != set.end())
         return false;

      // Past MostSpent the one that stops mattering soonest goes. Only finished
      // sign-ins are added, so a set this full is a server signing in some
      // twenty-seven people a second for ten minutes.
      while (set.size() >= MostSpent)
      {
         auto soonest = set.begin();
         for (auto it = set.begin(); it != set.end(); ++it)
         {
            if (it->second < soonest->second)
               soonest = it;
         }
         set.erase(soonest);
      }

      set[id] = expires;
      return true;
   }

   bool
   SignInState::IsSpent(const char *kind, const std::string &id)
   {
      std::lock_guard<std::mutex> guard(state_mutex);
      auto set = spent.find(kind);
      if (set == spent.end())
         return false;

      auto found = set->second.find(id);
      return found != set->second.end() && found->second >= time(nullptr);
   }

   void
   SignInState::Clear()
   {
      std::lock_guard<std::mutex> guard(state_mutex);
      MailboxCrypto::Forget(state_key);
      state_key.clear();
      spent.clear();
   }

   void
   SignInStateTester::Test()
   {
      auto expect = [](bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(what);
      };

      SignInState::Clear();

      const std::string plain = "v1\nstate\nverifier\nnonce\n0\n1790000000";
      const AnsiString sealed = SignInState::Seal(SignInState::Oidc, plain);
      expect(!sealed.IsEmpty(), "SignInStateTester: a state is sealed.");
      expect(sealed.Find("state") < 0 && sealed.Find("verifier") < 0,
             "SignInStateTester: what a sealed state holds cannot be read in it.");

      std::string opened;
      expect(SignInState::Open(SignInState::Oidc, sealed, opened) && opened == plain,
             "SignInStateTester: a sealed state opens to what was sealed.");
      expect(!SignInState::Open(SignInState::Passkey, sealed, opened),
             "SignInStateTester: a state sealed for one kind does not open as another.");

      // One character changed anywhere is refused, by the tag.
      AnsiString changed = sealed;
      changed.SetAt(changed.GetLength() / 2, changed.GetAt(changed.GetLength() / 2) == 'A' ? 'B' : 'A');
      expect(!SignInState::Open(SignInState::Oidc, changed, opened), "SignInStateTester: a changed state is refused.");
      expect(!SignInState::Open(SignInState::Oidc, "", opened) && !SignInState::Open(SignInState::Oidc, "not-a-state", opened),
             "SignInStateTester: text that is not a state is refused.");
      expect(!SignInState::Open(SignInState::Oidc, AnsiString(std::string(2000, 'A').c_str()), opened),
             "SignInStateTester: text longer than any state is refused.");

      // Two seals of one state differ (a fresh nonce each), and both open.
      const AnsiString again = SignInState::Seal(SignInState::Oidc, plain);
      expect(again != sealed && SignInState::Open(SignInState::Oidc, again, opened) && opened == plain,
             "SignInStateTester: each seal has a nonce of its own.");

      // Tags: the same data and kind, the same tag; anything else, another.
      const AnsiString tag = SignInState::Tag(SignInState::SamlRequest, "_abc|1|1790000000");
      expect(tag.GetLength() == 22, "SignInStateTester: a tag is sixteen bytes, base64url.");
      expect(SignInState::Verify(SignInState::SamlRequest, "_abc|1|1790000000", tag),
             "SignInStateTester: a tag verifies against what it was made of.");
      expect(!SignInState::Verify(SignInState::SamlRequest, "_abc|0|1790000000", tag) &&
             !SignInState::Verify(SignInState::Oidc, "_abc|1|1790000000", tag),
             "SignInStateTester: another datum, or another kind, does not verify.");

      // Spent once.
      const time_t later = time(nullptr) + 600;
      expect(!SignInState::IsSpent(SignInState::Oidc, "state-1"), "SignInStateTester: nothing is spent at first.");
      expect(SignInState::Spend(SignInState::Oidc, "state-1", later), "SignInStateTester: a state is spent the first time.");
      expect(!SignInState::Spend(SignInState::Oidc, "state-1", later) && SignInState::IsSpent(SignInState::Oidc, "state-1"),
             "SignInStateTester: and not again.");
      expect(SignInState::Spend(SignInState::Passkey, "state-1", later),
             "SignInStateTester: each kind keeps its own spent states.");

      // A Clear makes a new key: what was sealed before it no longer opens, and
      // nothing is spent.
      SignInState::Clear();
      expect(!SignInState::Open(SignInState::Oidc, sealed, opened),
             "SignInStateTester: a state sealed before a Clear is refused after it.");
      expect(!SignInState::Verify(SignInState::SamlRequest, "_abc|1|1790000000", tag),
             "SignInStateTester: and so is a tag made before it.");
      expect(!SignInState::IsSpent(SignInState::Oidc, "state-1"), "SignInStateTester: a Clear forgets what was spent.");

      SignInState::Clear();
   }
}
