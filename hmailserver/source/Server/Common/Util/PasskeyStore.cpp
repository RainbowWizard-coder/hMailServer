// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "PasskeyStore.h"
#include "SignInBudget.h"
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
         // Whom the challenge counts against when its table makes room: where
         // a sign-in challenge was asked for (SignInBudget::PendingSource), the
         // credential that asked for a registration's (the caller's identity);
         // and its group for the rule's tie-break (SignInBudget::SourceGroup).
         std::string source;
         std::string group;
      };

      boost::mutex challengeLock;
      std::map<AnsiString, PendingChallenge> registrations;
      std::map<AnsiString, PendingChallenge> signIns;
      unsigned __int64 nextOrder = 1;

      // A thousand registrations is far past any real load and is what stops a
      // script from filling memory by asking for challenges it never answers.
      // They are under SignInBudget's rule too, held by the credential that
      // asked: fifty a credential, as it holds fifty sessions, its own oldest
      // going past that, and a full table taking from whoever holds the most.
      // Until 6.3.6's re-check the oldest went whoever held it, so that one
      // account asking from a session it held or from address after address
      // of an IPv6 /64 pushed out every other person's registration while they
      // were answering their authenticator.
      const size_t MostPending = 1000;
      const size_t MostRegistrationsPerHolder = 50;

      // The sign-in challenges, which anybody may ask for, under SignInBudget's
      // rule: a source holds at most MostSignInsPerSource and gives up its own
      // oldest past that, and a full table gives up the oldest of the source
      // holding the most. Until 6.3.6 a thousand, the oldest dropped whoever
      // held it, which a hundred requests a second turned into nobody signing
      // in with a passkey at all.
      const size_t MostSignIns = 4096;
      const size_t MostSignInsPerSource = 256;

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

      // Room for one more challenge of holder; it never refuses the new one. The
      // sign-in challenge is handed out to anybody - there is no session yet,
      // which is the point of a passkey - so a table that refused when it filled
      // would be a switch a stranger could throw, and one that dropped its oldest
      // whoever held it was the same switch at a hundred requests a second
      // (SignInBudget.h). A holder spends its own slots: at its cap its own
      // oldest goes, and a full table gives up the oldest of the holder holding
      // the most, of the largest group among those tied. A holder of no source
      // of its own has no cap of its own.
      void MakeRoomLocked(std::map<AnsiString, PendingChallenge> &pending, const std::string &holder,
                          size_t perHolder, size_t most)
      {
         SignInBudget::MakeRoom(pending, holder,
            holder.empty() ? 0 : perHolder, most,
            [](const std::pair<const AnsiString, PendingChallenge> &entry) -> const std::string & { return entry.second.source; },
            [](const std::pair<const AnsiString, PendingChallenge> &entry) -> const std::string & { return entry.second.group; },
            [](const std::pair<const AnsiString, PendingChallenge> &entry) { return entry.second.order; },
            [](const std::pair<const AnsiString, PendingChallenge> &) { return false; });
      }

      bool RandomText(AnsiString &text, size_t bytes)
      {
         unsigned char buffer[64];
         if (bytes > sizeof(buffer) || RAND_bytes(buffer, (int) bytes) != 1)
            return false;
         text = WebAuthn::Base64Url(buffer, bytes);
         return true;
      }

      // holder: whom the challenge counts against (MakeRoomLocked), perHolder and
      // most the table's own numbers. A second challenge under the same key - a
      // session asking again - replaces the first before room is made, so it
      // never costs anybody else an entry.
      bool Offer(std::map<AnsiString, PendingChallenge> &pending, const AnsiString &key, AnsiString &challenge,
                 const std::string &holder, size_t perHolder, size_t most)
      {
         if (key.IsEmpty() || !RandomText(challenge, 32))
            return false;

         PendingChallenge entry;
         entry.challenge = challenge;
         entry.created = time(nullptr);
         entry.source = holder;
         entry.group = SignInBudget::SourceGroup(holder);

         boost::lock_guard<boost::mutex> guard(challengeLock);
         entry.order = nextOrder++;
         PruneLocked(pending, entry.created);
         pending.erase(key);
         MakeRoomLocked(pending, holder, perHolder, most);
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

   bool
   PasskeyStore::RelyingPartyConfigured()
   {
      AnsiString configured;
      Unicode::WideToMultiByte(Configuration::Instance()->GetSignInString(PROPERTY_PASSKEY_RELYING_PARTY), configured);
      configured.Trim();
      return !configured.IsEmpty();
   }

   std::vector<AnsiString>
   PasskeyStore::AllowedOrigins()
   {
      AnsiString configured;
      Unicode::WideToMultiByte(Configuration::Instance()->GetSignInString(PROPERTY_PASSKEY_ALLOWED_ORIGINS), configured);

      std::vector<AnsiString> origins;
      AnsiString one;
      for (int i = 0; i <= configured.GetLength(); i++)
      {
         const char c = i < configured.GetLength() ? configured[i] : ',';
         if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n')
         {
            one.Trim();
            while (one.EndsWith("/"))
               one = one.Mid(0, one.GetLength() - 1);
            one.MakeLower();
            if (!one.IsEmpty())
               origins.push_back(one);
            one = AnsiString();
            continue;
         }
         one += c;
      }
      return origins;
   }

   bool
   PasskeyStore::OriginAllowed(const AnsiString &origin, const AnsiString &originHost, const AnsiString &host,
                               AnsiString &problem)
   {
      if (originHost.IsEmpty())
      {
         problem = "the origin is not an https page, or plain http on a loopback name";
         return false;
      }

      const std::vector<AnsiString> allowed = AllowedOrigins();
      if (allowed.empty())
      {
         if (RelyingPartyConfigured())
         {
            problem = "PasskeyRelyingParty is set and PasskeyAllowedOrigins is not, so no origin is this server's own";
            return false;
         }

         AnsiString wantedHost = host;
         wantedHost.Trim();
         wantedHost.MakeLower();
         if (wantedHost.IsEmpty() || originHost != wantedHost)
         {
            problem = "the origin is not the host the request was sent to";
            return false;
         }
         return true;
      }

      AnsiString presented = origin;
      presented.Trim();
      while (presented.EndsWith("/"))
         presented = presented.Mid(0, presented.GetLength() - 1);
      presented.MakeLower();

      for (const AnsiString &one : allowed)
      {
         if (one == presented)
            return true;
      }

      problem = "the origin is not one PasskeyAllowedOrigins names";
      return false;
   }

   AnsiString
   PasskeyStore::MissingOriginsProblem()
   {
      if (!RelyingPartyConfigured() || !AllowedOrigins().empty())
         return AnsiString();
      return "passkey_relying_party is set, so passkey_allowed_origins must name the origins this server's pages are served at (https://mail.example.com); until it does, no passkey is registered or taken";
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
      // The parameters in the order the statement names them: SQL Server
      // Compact binds them by position, not by name, and with @ID added first
      // the account's id was compared with pkid and the passkey's with
      // pkaccountid - no row, and a 404 for a passkey the account holds.
      SQLCommand command(read);
      if (!administrator)
         command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@ID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;

      const String deletion = String(administrator
         ? "delete from hm_passkeys where pkaccountid is null and pkid = @ID"
         : "delete from hm_passkeys where pkaccountid = @ACCOUNTID and pkid = @ID");
      SQLCommand remove(deletion);
      if (!administrator)
         remove.AddParameter("@ACCOUNTID", accountId);
      remove.AddParameter("@ID", id);

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
   PasskeyStore::OfferRegistration(const std::string &holder, const AnsiString &sessionKey, AnsiString &challenge)
   {
      return Offer(registrations, sessionKey, challenge, holder, MostRegistrationsPerHolder, MostPending);
   }

   bool
   PasskeyStore::TakeRegistration(const AnsiString &sessionKey, AnsiString &challenge)
   {
      return Take(registrations, sessionKey, challenge);
   }

   bool
   PasskeyStore::OfferSignIn(const std::string &source, AnsiString &nonce, AnsiString &challenge)
   {
      if (!RandomText(nonce, 32))
         return false;
      return Offer(signIns, nonce, challenge, source, MostSignInsPerSource, MostSignIns);
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
      expect(PasskeyStore::OfferSignIn("198.51.100.4", nonce, challenge) && !nonce.IsEmpty() && !challenge.IsEmpty(),
             "PasskeyStoreTester: a sign-in challenge is handed out with its nonce.");
      expect(!PasskeyStore::TakeSignIn("not the nonce", taken),
             "PasskeyStoreTester: another nonce does not answer it.");
      expect(PasskeyStore::TakeSignIn(nonce, taken) && taken == challenge,
             "PasskeyStoreTester: its own nonce does.");
      expect(!PasskeyStore::TakeSignIn(nonce, taken),
             "PasskeyStoreTester: and it is spent, so an assertion cannot be replayed.");

      // A table a stranger floods still hands out challenges, and the flood
      // spends the stranger's own: a person's challenge, asked for before five
      // thousand from another source, still answers. Until 6.3.6 the table
      // dropped its oldest whoever held it, and the person's went.
      AnsiString personNonce, personChallenge;
      expect(PasskeyStore::OfferSignIn("198.51.100.4", personNonce, personChallenge), "PasskeyStoreTester: a person asks first.");

      AnsiString firstNonce, firstChallenge;
      expect(PasskeyStore::OfferSignIn("203.0.113.9", firstNonce, firstChallenge), "PasskeyStoreTester: the first of many.");

      for (int i = 0; i < 5000; i++)
      {
         AnsiString floodNonce, floodChallenge;
         expect(PasskeyStore::OfferSignIn("203.0.113.9", floodNonce, floodChallenge),
                "PasskeyStoreTester: a flooded table still hands out a challenge.");
      }

      AnsiString afterFlood, afterFloodChallenge, redeemed;
      expect(PasskeyStore::OfferSignIn("203.0.113.9", afterFlood, afterFloodChallenge),
             "PasskeyStoreTester: and one more after the flood.");
      expect(PasskeyStore::TakeSignIn(afterFlood, redeemed) && redeemed == afterFloodChallenge,
             "PasskeyStoreTester: which is the one that answers - the newest is kept.");
      expect(!PasskeyStore::TakeSignIn(firstNonce, redeemed),
             "PasskeyStoreTester: the flood's oldest is what went, the attacker's own slot.");
      expect(PasskeyStore::TakeSignIn(personNonce, redeemed) && redeemed == personChallenge,
             "PasskeyStoreTester: and the person's challenge, from another source, still answers.");

      // The registration table is the same table with a different key, and the
      // key there is the session rather than a cookie nonce.
      AnsiString registration;
      expect(PasskeyStore::OfferRegistration("account:person@example.test", "s:a-session", registration) && !registration.IsEmpty(),
             "PasskeyStoreTester: a registration challenge is held against the session that asked.");
      expect(!PasskeyStore::TakeRegistration("s:another-session", redeemed),
             "PasskeyStoreTester: another session cannot answer it.");
      expect(PasskeyStore::TakeRegistration("s:a-session", redeemed) && redeemed == registration,
             "PasskeyStoreTester: its own session can, once.");
      expect(!PasskeyStore::OfferRegistration("account:person@example.test", "", redeemed),
             "PasskeyStoreTester: a challenge with no key to hold it against is not made.");

      // One credential asking for registration after registration under keys of
      // its own - session after session, or address after address - spends its
      // own fifty and nobody else's: a person's registration, asked for first,
      // still answers. Until 6.3.6's re-check the table dropped its oldest
      // whoever held it, and the person's went.
      AnsiString personRegistration;
      expect(PasskeyStore::OfferRegistration("account:person@example.test", "s:person", personRegistration),
             "PasskeyStoreTester: a person asks for a registration.");
      for (int i = 0; i < 1200; i++)
      {
         AnsiString floodRegistration;
         expect(PasskeyStore::OfferRegistration("account:flood@example.test", "c:account:flood@example.test@2001:db8::" + AnsiString(std::to_string(i + 1).c_str()), floodRegistration),
                "PasskeyStoreTester: a flooded registration table still hands out a challenge.");
      }
      expect(PasskeyStore::TakeRegistration("s:person", redeemed) && redeemed == personRegistration,
             "PasskeyStoreTester: and the person's registration, another credential's, still answers.");
      expect(!PasskeyStore::TakeRegistration("c:account:flood@example.test@2001:db8::1", redeemed) &&
             PasskeyStore::TakeRegistration("c:account:flood@example.test@2001:db8::1200", redeemed),
             "PasskeyStoreTester: the flooding credential's oldest went, and its newest answers.");

      PasskeyStore::Clear();
   }
}
