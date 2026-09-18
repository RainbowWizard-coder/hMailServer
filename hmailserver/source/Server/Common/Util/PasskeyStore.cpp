// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "PasskeyStore.h"
#include "WebAuthn.h"
#include "Unicode.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>

#include <cstdlib>
#include <ctime>
#include <map>
#include <stdexcept>
#include <string>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      struct PendingChallenge
      {
         AnsiString challenge;
         time_t created = 0;
         // Which challenge this was, counted under the lock. created is only
         // accurate to the second, and a flood makes a thousand of these inside
         // one, so this is what "the oldest" means when the table is full.
         unsigned __int64 order = 0;
      };

      boost::mutex challengeLock;
      std::map<AnsiString, PendingChallenge> registrations;
      std::map<AnsiString, PendingChallenge> signIns;
      unsigned __int64 nextOrder = 1;

      // A thousand of each is far past any real load and is what stops a script
      // from filling memory by asking for challenges it never answers.
      const size_t MostPending = 1000;

      void PruneLocked(std::map<AnsiString, PendingChallenge> &pending, time_t now)
      {
         for (auto it = pending.begin(); it != pending.end();)
         {
            if (now - it->second.created > PasskeyStore::ChallengeSeconds)
               it = pending.erase(it);
            else
               ++it;
         }
      }

      // A full table drops its OLDEST entry and takes the new one; it does not
      // refuse the new one. The sign-in challenge is handed out to anybody -
      // there is no session yet, which is the point of a passkey - so a table
      // that refused when it filled would be a switch a stranger could throw:
      // a thousand requests, a second's work, and nobody can sign in with a
      // passkey for the next five minutes. Evicting turns that into an
      // attacker spending their own slots, which is what browser_sessions and
      // the rate windows in RestApiServer.cpp already do for the same reason.
      void MakeRoomLocked(std::map<AnsiString, PendingChallenge> &pending)
      {
         while (pending.size() >= MostPending)
         {
            auto oldest = pending.begin();
            for (auto it = pending.begin(); it != pending.end(); ++it)
            {
               if (it->second.order < oldest->second.order)
                  oldest = it;
            }
            pending.erase(oldest);
         }
      }

      bool RandomText(AnsiString &text, size_t bytes)
      {
         unsigned char buffer[64];
         if (bytes > sizeof(buffer) || RAND_bytes(buffer, (int) bytes) != 1)
            return false;
         text = WebAuthn::Base64Url(buffer, bytes);
         return true;
      }

      bool Offer(std::map<AnsiString, PendingChallenge> &pending, const AnsiString &key, AnsiString &challenge)
      {
         if (key.IsEmpty() || !RandomText(challenge, 32))
            return false;

         PendingChallenge entry;
         entry.challenge = challenge;
         entry.created = time(nullptr);

         boost::lock_guard<boost::mutex> guard(challengeLock);
         entry.order = nextOrder++;
         PruneLocked(pending, entry.created);
         MakeRoomLocked(pending);
         pending[key] = entry;
         return true;
      }

      // Spent on first use, whether or not what follows succeeds: a challenge
      // that could be answered twice is a replayed assertion waiting to happen.
      bool Take(std::map<AnsiString, PendingChallenge> &pending, const AnsiString &key, AnsiString &challenge)
      {
         if (key.IsEmpty())
            return false;

         boost::lock_guard<boost::mutex> guard(challengeLock);
         const time_t now = time(nullptr);
         PruneLocked(pending, now);

         auto it = pending.find(key);
         if (it == pending.end())
            return false;

         challenge = it->second.challenge;
         pending.erase(it);
         return !challenge.IsEmpty();
      }

      PasskeyStore::Passkey ReadRow(std::shared_ptr<DALRecordset> recordset)
      {
         PasskeyStore::Passkey passkey;
         passkey.id = recordset->GetInt64Value("pkid");
         // A null account id is the administrator's own row, which is why the
         // column is nullable at all: the administrator is a password in
         // hMailServer.ini and has no row in hm_accounts to point at.
         passkey.administrator = recordset->GetIsNull("pkaccountid");
         passkey.account_id = passkey.administrator ? 0 : recordset->GetInt64Value("pkaccountid");
         passkey.credential = AnsiString(recordset->GetStringValue("pkcredential"));
         passkey.public_key = AnsiString(recordset->GetStringValue("pkpublickey"));
         passkey.algorithm = (int) recordset->GetLongValue("pkalgorithm");
         passkey.sign_count = recordset->GetInt64Value("pksigncount");
         passkey.name = recordset->GetStringValue("pkname");
         passkey.transports = recordset->GetStringValue("pktransports");
         passkey.aaguid = AnsiString(recordset->GetStringValue("pkaaguid"));
         passkey.created = recordset->GetInt64Value("pkcreated");
         passkey.last_used = recordset->GetInt64Value("pklastused");
         return passkey;
      }

      const char *Columns =
         "pkid, pkaccountid, pkcredential, pkpublickey, pkalgorithm, pksigncount, "
         "pkname, pktransports, pkaaguid, pkcreated, pklastused";
   }

   bool
   PasskeyStore::Enabled()
   {
      return Configuration::Instance()->GetPasskeysEnabled();
   }

   AnsiString
   PasskeyStore::RelyingParty(const AnsiString &host)
   {
      AnsiString configured;
      Unicode::WideToMultiByte(Configuration::Instance()->GetSignInString(PROPERTY_PASSKEY_RELYING_PARTY), configured);
      configured.Trim();
      if (!configured.IsEmpty())
         return configured;

      AnsiString name = host;
      name.Trim();

      // An IPv6 literal is written [::1]:8080; everything else is host:port.
      if (name.StartsWith("["))
      {
         const int close = name.Find("]");
         if (close < 0)
            return "";
         name = name.Mid(1, close - 1);
      }
      else
      {
         const int colon = name.Find(":");
         if (colon >= 0)
            name = name.Mid(0, colon);
      }

      name.MakeLower();

      // A relying party id is a domain name. The browser refuses anything else,
      // so a host this server cannot make one of is refused here instead, with
      // a sentence, rather than in the browser with a DOMException.
      if (name.IsEmpty() || name.GetLength() > 253)
         return "";
      const char *characters = name.c_str();
      for (int i = 0; i < name.GetLength(); i++)
      {
         const char c = characters[i];
         const bool allowed = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
         if (!allowed)
            return "";
      }

      return name;
   }

   AnsiString
   PasskeyStore::UserHandle(__int64 accountId, bool administrator)
   {
      AnsiString text;
      if (administrator)
         text = "admin";
      else
         text.Format("a%I64d", accountId);

      return WebAuthn::Base64Url((const unsigned char *) text.c_str(), (size_t) text.GetLength());
   }

   bool
   PasskeyStore::OwnerOfHandle(const std::string &handle, __int64 &accountId, bool &administrator)
   {
      accountId = 0;
      administrator = false;

      std::string text;
      if (!WebAuthn::Base64UrlDecode(handle, text) || text.empty() || text.size() > 24)
         return false;

      if (text == "admin")
      {
         administrator = true;
         return true;
      }

      if (text[0] != 'a' || text.size() < 2)
         return false;

      for (size_t i = 1; i < text.size(); i++)
      {
         if (text[i] < '0' || text[i] > '9')
            return false;
      }

      accountId = (__int64) strtoll(text.c_str() + 1, nullptr, 10);
      return accountId > 0;
   }

   bool
   PasskeyStore::List(__int64 accountId, bool administrator, std::vector<Passkey> &passkeys)
   {
      passkeys.clear();

      const String query = String(AnsiString("select ") + Columns + (administrator
         ? " from hm_passkeys where pkaccountid is null order by pkid desc"
         : " from hm_passkeys where pkaccountid = @ACCOUNTID order by pkid desc"));
      SQLCommand command(query);
      if (!administrator)
         command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         passkeys.push_back(ReadRow(recordset));
         recordset->MoveNext();
      }
      return true;
   }

   bool
   PasskeyStore::FindByCredential(const AnsiString &credential, Passkey &passkey)
   {
      if (credential.IsEmpty() || credential.GetLength() > 200)
         return false;

      const String query = String(AnsiString("select ") + Columns + " from hm_passkeys where pkcredential = @CREDENTIAL");
      SQLCommand command(query);
      command.AddParameter("@CREDENTIAL", String(credential));

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      passkey = ReadRow(recordset);
      return true;
   }

   bool
   PasskeyStore::Add(Passkey &passkey, String &problem)
   {
      if (passkey.credential.GetLength() > 200)
      {
         problem = _T("the authenticator's credential id is longer than this server stores (200 characters of base64url)");
         return false;
      }

      Passkey existing;
      if (FindByCredential(passkey.credential, existing))
      {
         problem = existing.account_id == passkey.account_id && existing.administrator == passkey.administrator
            ? _T("that device is already registered here")
            : _T("that credential is registered to somebody else on this server");
         return false;
      }

      std::vector<Passkey> already;
      if (!List(passkey.account_id, passkey.administrator, already))
      {
         problem = _T("the passkeys could not be read");
         return false;
      }
      if ((int) already.size() >= MostPerOwner)
      {
         problem.Format(_T("at most %d passkeys; remove one first"), MostPerOwner);
         return false;
      }

      SQLStatement statement(SQLStatement::STInsert, "hm_passkeys");
      if (passkey.administrator)
         statement.AddColumnNULL("pkaccountid");
      else
         statement.AddColumnInt64("pkaccountid", passkey.account_id);
      statement.AddColumn("pkcredential", String(passkey.credential));
      statement.AddColumn("pkpublickey", String(passkey.public_key));
      statement.AddColumn("pkalgorithm", (long) passkey.algorithm);
      statement.AddColumnInt64("pksigncount", passkey.sign_count);
      statement.AddColumn("pkname", passkey.name, 255);
      statement.AddColumn("pktransports", passkey.transports, 255);
      statement.AddColumn("pkaaguid", String(passkey.aaguid), 64);
      statement.AddColumnInt64("pkcreated", passkey.created);
      statement.AddColumnInt64("pklastused", passkey.last_used);
      statement.SetIdentityColumn("pkid");

      __int64 made = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &made) || made <= 0)
      {
         problem = _T("the passkey could not be saved; see the error log");
         return false;
      }

      passkey.id = made;
      return true;
   }

   bool
   PasskeyStore::Remove(__int64 accountId, bool administrator, __int64 id)
   {
      if (id <= 0)
         return false;

      // Read first: the answer has to be "not one of yours" rather than "done"
      // when the id belongs to somebody else, and the delete below is then
      // scoped by the same owner clause so that a race cannot widen it.
      const String read = String(administrator
         ? "select pkid from hm_passkeys where pkaccountid is null and pkid = @ID"
         : "select pkid from hm_passkeys where pkaccountid = @ACCOUNTID and pkid = @ID");
      SQLCommand command(read);
      command.AddParameter("@ID", id);
      if (!administrator)
         command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      const String deletion = String(administrator
         ? "delete from hm_passkeys where pkaccountid is null and pkid = @ID"
         : "delete from hm_passkeys where pkaccountid = @ACCOUNTID and pkid = @ID");
      SQLCommand remove(deletion);
      remove.AddParameter("@ID", id);
      if (!administrator)
         remove.AddParameter("@ACCOUNTID", accountId);

      return Application::Instance()->GetDBManager()->Execute(remove);
   }

   void
   PasskeyStore::RecordUse(__int64 id, __int64 signCount)
   {
      SQLCommand command("update hm_passkeys set pksigncount = @COUNT, pklastused = @NOW where pkid = @ID");
      command.AddParameter("@COUNT", signCount);
      command.AddParameter("@NOW", (__int64) time(nullptr));
      command.AddParameter("@ID", id);
      Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   PasskeyStore::OfferRegistration(const AnsiString &sessionKey, AnsiString &challenge)
   {
      return Offer(registrations, sessionKey, challenge);
   }

   bool
   PasskeyStore::TakeRegistration(const AnsiString &sessionKey, AnsiString &challenge)
   {
      return Take(registrations, sessionKey, challenge);
   }

   bool
   PasskeyStore::OfferSignIn(AnsiString &nonce, AnsiString &challenge)
   {
      if (!RandomText(nonce, 32))
         return false;
      return Offer(signIns, nonce, challenge);
   }

   bool
   PasskeyStore::TakeSignIn(const AnsiString &nonce, AnsiString &challenge)
   {
      return Take(signIns, nonce, challenge);
   }

   void
   PasskeyStore::Clear()
   {
      boost::lock_guard<boost::mutex> guard(challengeLock);
      registrations.clear();
      signIns.clear();
   }

   // ------------------------------------------------------------------------
   // The self-test. No database is touched: everything below is the in-memory
   // challenge tables, which are the part an unauthenticated stranger reaches.
   // ------------------------------------------------------------------------
   void
   PasskeyStoreTester::Test()
   {
      auto expect = [](bool condition, const char *what)
      {
         if (!condition)
            throw std::logic_error(what);
      };

      PasskeyStore::Clear();

      // A challenge is answerable once, by the nonce it was handed out with.
      AnsiString nonce, challenge, taken;
      expect(PasskeyStore::OfferSignIn(nonce, challenge) && !nonce.IsEmpty() && !challenge.IsEmpty(),
             "PasskeyStoreTester: a sign-in challenge is handed out with its nonce.");
      expect(!PasskeyStore::TakeSignIn("not the nonce", taken),
             "PasskeyStoreTester: another nonce does not answer it.");
      expect(PasskeyStore::TakeSignIn(nonce, taken) && taken == challenge,
             "PasskeyStoreTester: its own nonce does.");
      expect(!PasskeyStore::TakeSignIn(nonce, taken),
             "PasskeyStoreTester: and it is spent, so an assertion cannot be replayed.");

      // A full table evicts its oldest entry rather than refusing the newest.
      // A thousand unanswered challenges is what a stranger can make in about a
      // second, and the person who presses the button afterwards must still be
      // handed one.
      AnsiString firstNonce, firstChallenge;
      expect(PasskeyStore::OfferSignIn(firstNonce, firstChallenge), "PasskeyStoreTester: the first of many.");

      for (int i = 0; i < 1200; i++)
      {
         AnsiString floodNonce, floodChallenge;
         expect(PasskeyStore::OfferSignIn(floodNonce, floodChallenge),
                "PasskeyStoreTester: a full table still hands out a challenge.");
      }

      AnsiString afterFlood, afterFloodChallenge, redeemed;
      expect(PasskeyStore::OfferSignIn(afterFlood, afterFloodChallenge),
             "PasskeyStoreTester: and one more after the flood.");
      expect(PasskeyStore::TakeSignIn(afterFlood, redeemed) && redeemed == afterFloodChallenge,
             "PasskeyStoreTester: which is the one that answers - the newest is kept.");
      expect(!PasskeyStore::TakeSignIn(firstNonce, redeemed),
             "PasskeyStoreTester: the oldest is what went, which is the attacker's own slot and not the next person's.");

      // The registration table is the same table with a different key, and the
      // key there is the session rather than a cookie nonce.
      AnsiString registration;
      expect(PasskeyStore::OfferRegistration("s:a-session", registration) && !registration.IsEmpty(),
             "PasskeyStoreTester: a registration challenge is held against the session that asked.");
      expect(!PasskeyStore::TakeRegistration("s:another-session", redeemed),
             "PasskeyStoreTester: another session cannot answer it.");
      expect(PasskeyStore::TakeRegistration("s:a-session", redeemed) && redeemed == registration,
             "PasskeyStoreTester: its own session can, once.");
      expect(!PasskeyStore::OfferRegistration("", redeemed),
             "PasskeyStoreTester: a challenge with no key to hold it against is not made.");

      PasskeyStore::Clear();
   }
}
