// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The REST API's OpenPGP public keys. See OpenPgpKeyStore.h, OpenPgpKey.h and
// docs/OpenPGP.md; the serving itself is on the web services listener
// (WebServicesServer.cpp), not here.
//
// The account's own, which is where a key normally arrives from:
//
//    GET    /api/v1/me/openpgp                  the keys, the switch, the URLs
//    PUT    /api/v1/me/openpgp                  {"key": "..."} - upload one
//    DELETE /api/v1/me/openpgp/{fingerprint}    remove one
//    PUT    /api/v1/me/openpgp/publish          {"publish": true|false}
//
// An administrator's, for the person who keeps their key in a client of their
// own and hands the public half to whoever runs the server:
//
//    GET    /api/v1/accounts/{address}/openpgp                the same document
//    PUT    /api/v1/accounts/{address}/openpgp                {"key", "allow_address_mismatch"?, "publish"?}
//    DELETE /api/v1/accounts/{address}/openpgp/{fingerprint}
//    GET    /api/v1/domains/{domain}/openpgp                  every key of the domain
//
// ONLY PUBLIC KEYS. Every one of these goes through OpenPgpKeyStore::Store,
// which parses before it stores and refuses a secret key with a sentence saying
// so. There is no route, and no other code path, that puts a secret key in this
// server.
//
// allow_address_mismatch waives one rule - that the key's user ids carry the
// account's own address - and only an administrator has it. The account's own
// PUT has no such field: a person publishing a key under their own address that
// names somebody else's is the mistake the rule exists to catch.
//
// The account's own three writes are the account holder's alone
// (RefuseUnlessAccountHolder_): refused to an app password, a mail program's
// credential, and to an administrator's support session, which acts as the
// person to help them. The key published here is the one every correspondent
// who looks the address up encrypts to; either credential could otherwise put
// its own key there and read what is sent to the person. An administrator who
// keeps a person's key for them uses the administrator's routes below, under
// manage_accounts, where the change is on the record as theirs.
#include "StdAfx.h"

#include "RestApiServer.h"
#include "HttpServer.h"
#include "JsonDocument.h"
#include "OpenPgpKeyStore.h"
#include "Unicode.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../BO/Domains.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentAccount.h"

#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The key as a client sends it: an armoured PGP PUBLIC KEY BLOCK or base64
      // of the key's bytes. Held to a generous ceiling here so that a body nobody
      // could mean is refused before the parser sees it; the real cap is
      // OpenPGPKeyMaximumBytes, applied to the KEY and not to its armour.
      const size_t KeyTextMaximum = 4 * 1024 * 1024;

      bool ReadKeyText(const AnsiString &requestBody, AnsiString &keyText, AnsiString &problem)
      {
         JsonValue document;
         std::string parseError;
         if (!JsonValue::Parse(std::string(requestBody.c_str(), (size_t) requestBody.GetLength()), document, parseError) || !document.IsObject())
         {
            problem = "{\"error\":\"a JSON object with a key member is expected\"}";
            return false;
         }

         const JsonValue *key = document.Get("key");
         if (!key || !key->IsString() || key->AsString().empty())
         {
            problem = "{\"error\":\"key: the OpenPGP public key, armoured or as base64 of its bytes\"}";
            return false;
         }

         if (key->AsString().size() > KeyTextMaximum)
         {
            problem = "{\"error\":\"key: larger than 4 MB of text, which is not a key\"}";
            return false;
         }

         keyText = AnsiString(key->AsString().c_str());
         return true;
      }

      bool ReadFlag(const AnsiString &requestBody, const char *name, bool &value, bool &present)
      {
         present = false;

         JsonValue document;
         std::string parseError;
         if (!JsonValue::Parse(std::string(requestBody.c_str(), (size_t) requestBody.GetLength()), document, parseError) || !document.IsObject())
            return false;

         const JsonValue *member = document.Get(name);
         if (!member || member->IsNull())
            return true;

         if (!member->IsBool())
            return false;

         value = member->AsBool();
         present = true;
         return true;
      }

      std::shared_ptr<const Account> AccountByAddress(const AnsiString &address)
      {
         String wanted;
         Unicode::MultiByteToWide(address, wanted);
         wanted.Trim();
         wanted.ToLower();

         if (wanted.IsEmpty())
            return std::shared_ptr<const Account>();

         return CacheContainer::Instance()->GetAccount(wanted);
      }
   }

   // ---- the account's own -------------------------------------------------

   HttpResponse
   RestApiServer::HandleMeOpenPgp_(const Caller &caller)
   {
      if (!caller.account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      return BuildResponse_(200, OpenPgpKeyStore::AccountJson(caller.account));
   }

   HttpResponse
   RestApiServer::HandleMeOpenPgpPut_(const Caller &caller, const AnsiString &requestBody)
   {
      if (!caller.account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      HttpResponse refusal;
      if (RefuseUnlessAccountHolder_(caller, "publishing an OpenPGP key", refusal))
         return refusal;

      AnsiString keyText, problem;
      if (!ReadKeyText(requestBody, keyText, problem))
         return BuildResponse_(400, problem);

      OpenPgpKeyStore::Row row;
      bool replaced = false;
      if (!OpenPgpKeyStore::Store(caller.account, keyText, false, caller.account->GetAddress(), row, replaced, problem))
      {
         AnsiString json;
         json.Format("{\"error\":\"%hs\"}", JsonEscape_(problem).c_str());
         return BuildResponse_(400, json);
      }

      return BuildResponse_(replaced ? 200 : 201, OpenPgpKeyStore::RowJson(row, caller.account->GetAddress(), false));
   }

   HttpResponse
   RestApiServer::HandleMeOpenPgpDelete_(const Caller &caller, const AnsiString &fingerprint)
   {
      if (!caller.account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      HttpResponse refusal;
      if (RefuseUnlessAccountHolder_(caller, "removing a published OpenPGP key", refusal))
         return refusal;

      if (!OpenPgpKeyStore::Remove(caller.account->GetID(), fingerprint))
         return BuildResponse_(404, "{\"error\":\"this account holds no key with that fingerprint\"}");

      return BuildResponse_(200, "{\"removed\":true}");
   }

   HttpResponse
   RestApiServer::HandleMeOpenPgpPublish_(const Caller &caller, const AnsiString &requestBody)
   {
      if (!caller.account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      HttpResponse refusal;
      if (RefuseUnlessAccountHolder_(caller, "switching OpenPGP key publication", refusal))
         return refusal;

      bool publish = false;
      bool present = false;
      if (!ReadFlag(requestBody, "publish", publish, present) || !present)
         return BuildResponse_(400, "{\"error\":\"publish: true or false\"}");

      AnsiString problem;
      if (!SetAccountOpenPgpPublish_(caller.account->GetID(), publish, problem))
      {
         AnsiString json;
         json.Format("{\"error\":\"%hs\"}", JsonEscape_(problem).c_str());
         return BuildResponse_(500, json);
      }

      std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(caller.account->GetID());
      return BuildResponse_(200, OpenPgpKeyStore::AccountJson(account ? account : caller.account));
   }

   // ---- the administrator's ------------------------------------------------

   HttpResponse
   RestApiServer::HandleAccountOpenPgp_(const AnsiString &address)
   {
      std::shared_ptr<const Account> account = AccountByAddress(address);
      if (!account)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      return BuildResponse_(200, OpenPgpKeyStore::AccountJson(account));
   }

   HttpResponse
   RestApiServer::HandleAccountOpenPgpPut_(const Caller &caller, const AnsiString &address, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = AccountByAddress(address);
      if (!account)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      AnsiString keyText, problem;
      if (!ReadKeyText(requestBody, keyText, problem))
         return BuildResponse_(400, problem);

      bool allowMismatch = false;
      bool mismatchGiven = false;
      if (!ReadFlag(requestBody, "allow_address_mismatch", allowMismatch, mismatchGiven))
         return BuildResponse_(400, "{\"error\":\"allow_address_mismatch: true or false\"}");

      // The waiver is the server administrator's. A credential confined to
      // domains may upload a key for an account of one of them, and may not
      // publish under that address a key that names somebody else.
      if (allowMismatch && !caller.domains.empty())
         return BuildResponse_(403, "{\"error\":\"allow_address_mismatch is refused to a credential confined to domains: only a server administrator may publish a key whose user ids do not carry the account's own address\"}");

      bool publish = false;
      bool publishGiven = false;
      if (!ReadFlag(requestBody, "publish", publish, publishGiven))
         return BuildResponse_(400, "{\"error\":\"publish: true or false\"}");

      OpenPgpKeyStore::Row row;
      bool replaced = false;
      if (!OpenPgpKeyStore::Store(account, keyText, allowMismatch, String(caller.identity), row, replaced, problem))
      {
         AnsiString json;
         json.Format("{\"error\":\"%hs\"}", JsonEscape_(problem).c_str());
         return BuildResponse_(400, json);
      }

      // publish is optional and deliberately separate from the upload: storing a
      // key for somebody is not the same as telling the world about it, and an
      // administrator has to say so for it to happen.
      if (publishGiven)
      {
         if (!SetAccountOpenPgpPublish_(account->GetID(), publish, problem))
         {
            AnsiString json;
            json.Format("{\"error\":\"the key was stored and the publish switch could not be saved: %hs\"}", JsonEscape_(problem).c_str());
            return BuildResponse_(500, json);
         }
      }

      return BuildResponse_(replaced ? 200 : 201, OpenPgpKeyStore::RowJson(row, account->GetAddress(), false));
   }

   HttpResponse
   RestApiServer::HandleAccountOpenPgpDelete_(const AnsiString &address, const AnsiString &fingerprint)
   {
      std::shared_ptr<const Account> account = AccountByAddress(address);
      if (!account)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      if (!OpenPgpKeyStore::Remove(account->GetID(), fingerprint))
         return BuildResponse_(404, "{\"error\":\"this account holds no key with that fingerprint\"}");

      return BuildResponse_(200, "{\"removed\":true}");
   }

   HttpResponse
   RestApiServer::HandleDomainOpenPgpList_(const AnsiString &domainName)
   {
      Domains domains;
      domains.Refresh();

      String wanted;
      Unicode::MultiByteToWide(domainName, wanted);
      wanted.Trim();
      wanted.ToLower();

      std::shared_ptr<const Domain> domain = domains.GetItemByName(wanted);
      if (!domain)
         return BuildResponse_(404, "{\"error\":\"no such domain\"}");

      std::vector<OpenPgpKeyStore::DomainKey> keys;
      OpenPgpKeyStore::ListDomain(domain, keys);

      const bool serverEnabled = Configuration::Instance()->GetOpenPGPWebKeyDirectoryEnabled();
      const bool domainEnabled = domain->GetIsActive() && domain->GetOpenPGPPublish();

      AnsiString json;
      json.Format("{\"domain\":\"%hs\",\"server_enabled\":%hs,\"domain_enabled\":%hs,\"publish_new_accounts\":%hs,\"total\":%d,\"keys\":[",
         JsonEscape_(Utf8_(domain->GetName())).c_str(),
         serverEnabled ? "true" : "false",
         domainEnabled ? "true" : "false",
         domain->GetOpenPGPPublishNewAccounts() ? "true" : "false",
         (int) keys.size());

      for (size_t i = 0; i < keys.size(); i++)
      {
         if (i > 0)
            json += ",";

         // The row, with the address and the two URLs a client would use, and
         // whether this key is actually being served - which is the question an
         // administrator is looking at this list to answer.
         AnsiString row = OpenPgpKeyStore::RowJson(keys[i].row, keys[i].address, false);
         row = row.Mid(0, row.GetLength() - 1);
         row += ",\"published\":";
         row += (serverEnabled && domainEnabled && keys[i].published) ? "true" : "false";
         row += "}";
         json += row;
      }

      json += "]}";
      return BuildResponse_(200, json);
   }

   // The account's publish switch, read fresh and saved, for the two routes that
   // change it. Read fresh because the caller's Account is the cached one and a
   // save writes every column of the object it is given.
   bool
   RestApiServer::SetAccountOpenPgpPublish_(__int64 accountId, bool publish, AnsiString &problem)
   {
      std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(account, accountId) || account->GetID() == 0)
      {
         problem = "the account could not be read";
         return false;
      }

      account->SetOpenPGPPublish(publish);

      String saveError;
      if (!PersistentAccount::SaveObject(account, saveError, PersistenceModeNormal))
      {
         problem = saveError.IsEmpty() ? AnsiString("the account could not be saved") : Utf8_(saveError);
         return false;
      }

      return true;
   }

   AnsiString
   RestApiServer::OpenApiOpenPgpPaths_()
   {
      static const char *paths =
         ",\"/api/v1/me/openpgp\":{"
         "\"get\":{\"summary\":\"The signed-in account's OpenPGP public keys\","
         "\"description\":\"address, publish (this account's switch), server_enabled and domain_enabled (the two switches above it), published (whether anything is actually served: all three, and at least one key), hashed_local_part, advanced_url and direct_url (where a client looks, draft-koch-openpgp-webkey-service), limit, and keys - each id, fingerprint, version, algorithm, bits, user_id, created, expires, bytes, uploaded and uploaded_by. No secret key is ever held, so none is ever reported.\","
         "\"responses\":{\"200\":{\"description\":\"The document\"}}},"
         "\"put\":{\"summary\":\"Upload the account's own OpenPGP public key\","
         "\"description\":\"Body: key - an armoured PGP PUBLIC KEY BLOCK or base64 of the key's bytes. The bytes are parsed (RFC 9580 packet framing, a primary public-key packet, at least one user id and one self-signature, the packets adding up) and the fingerprint computed for the key's version. A SECRET key is refused with a sentence saying so, and so is a key with no user id, a key with no self-signature, a keyring, a truncated key, a key past OpenPGPKeyMaximumBytes and a key whose user ids do not carry this account's address. A key this account already holds by that fingerprint is replaced and answered 200 rather than refused. At most OpenPGPKeysPerAccount keys. Refused (403) to an app password and to an administrator's support session: the key published here is what correspondents encrypt to.\","
         "\"requestBody\":{\"required\":true,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"key\"],\"properties\":{\"key\":{\"type\":\"string\"}}}}}},"
         "\"responses\":{\"201\":{\"description\":\"Stored; the key's row\"},\"200\":{\"description\":\"Replaced; the key's row\"},\"400\":{\"description\":\"The refusal, in one sentence\"},\"403\":{\"description\":\"An app password, or an administrator's support session\"}}}},"
         "\"/api/v1/me/openpgp/{fingerprint}\":{\"delete\":{\"summary\":\"Remove one of the account's own keys\","
         "\"description\":\"By the fingerprint the listing shows, lower-case hex. A key of another account is not found here. Refused (403) to an app password and to an administrator's support session.\","
         "\"parameters\":[{\"name\":\"fingerprint\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"removed true\"},\"404\":{\"description\":\"This account holds no key with that fingerprint\"},\"403\":{\"description\":\"An app password, or an administrator's support session\"}}}},"
         "\"/api/v1/me/openpgp/publish\":{\"put\":{\"summary\":\"Publish this account's keys, or stop\","
         "\"description\":\"Body: publish, true or false. The account's own switch and nobody else's business: holding a key so that a mail client can find it is not the same decision as telling the whole internet about it. The server setting and the domain's switch still have to be on for anything to be served, and the answer says whether they are. Refused (403) to an app password and to an administrator's support session.\","
         "\"requestBody\":{\"required\":true,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"publish\"],\"properties\":{\"publish\":{\"type\":\"boolean\"}}}}}},"
         "\"responses\":{\"200\":{\"description\":\"The document GET answers\"},\"400\":{\"description\":\"publish was not true or false\"},\"403\":{\"description\":\"An app password, or an administrator's support session\"}}}},"
         "\"/api/v1/accounts/{address}/openpgp\":{"
         "\"get\":{\"summary\":\"An account's OpenPGP public keys (administrator)\","
         "\"description\":\"The same document the account's own GET /api/v1/me/openpgp answers. A key restricted to named domains reaches the accounts of those domains only.\","
         "\"parameters\":[{\"name\":\"address\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"The document\"},\"404\":{\"description\":\"No such account\"}}},"
         "\"put\":{\"summary\":\"Upload an OpenPGP public key for an account (administrator)\","
         "\"description\":\"Body: key (required, as the account's own PUT takes it); allow_address_mismatch, which waives the rule that the key's user ids carry the account's own address and is refused to a credential confined to domains; and publish, which sets the account's switch in the same request. A SECRET key is refused here as everywhere. Recorded on the audit trail as an OpenPGP key, with the administrator who uploaded it.\","
         "\"parameters\":[{\"name\":\"address\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"requestBody\":{\"required\":true,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"key\"],\"properties\":{\"key\":{\"type\":\"string\"},\"allow_address_mismatch\":{\"type\":\"boolean\"},\"publish\":{\"type\":\"boolean\"}}}}}},"
         "\"responses\":{\"201\":{\"description\":\"Stored; the key's row\"},\"200\":{\"description\":\"Replaced; the key's row\"},\"400\":{\"description\":\"The refusal, in one sentence\"},\"403\":{\"description\":\"allow_address_mismatch from a credential confined to domains\"},\"404\":{\"description\":\"No such account\"}}}},"
         "\"/api/v1/accounts/{address}/openpgp/{fingerprint}\":{\"delete\":{\"summary\":\"Remove an account's OpenPGP key (administrator)\","
         "\"description\":\"By fingerprint, through the account, so a key of another account is not found here. Recorded on the audit trail.\","
         "\"parameters\":[{\"name\":\"address\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}},{\"name\":\"fingerprint\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"removed true\"},\"404\":{\"description\":\"No such account, or it holds no key with that fingerprint\"}}}},"
         "\"/api/v1/domains/{domain}/openpgp\":{\"get\":{\"summary\":\"Every OpenPGP key held for a domain's accounts (administrator)\","
         "\"description\":\"domain, server_enabled, domain_enabled, publish_new_accounts, total and keys - each the key's row with the account's address, its hashed_local_part, advanced_url and direct_url, and published: whether this key is actually being served. Keys that are NOT being served are listed too, which is what an administrator is looking here to find out. The domain's own switches are written through PUT /api/v1/domains/{domain} as openpgp_publish and openpgp_publish_new_accounts.\","
         "\"parameters\":[{\"name\":\"domain\",\"in\":\"path\",\"required\":true,\"schema\":{\"type\":\"string\"}}],"
         "\"responses\":{\"200\":{\"description\":\"The listing\"},\"404\":{\"description\":\"No such domain\"}}}}";

      return AnsiString(paths);
   }
}
