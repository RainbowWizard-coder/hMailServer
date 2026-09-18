// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// OpenPGP for the webmail: the account's key ring, the one lookup a browser
// cannot make for itself, and the module the page does the cryptography in.
//
// The cryptography is the page's (PortalOpenPgp.js, served as
// /portal-openpgp.js from PortalOpenPgpData.cpp): keys are made and imported,
// signatures made and checked, messages encrypted and decrypted in the browser
// on the Web Crypto API, BigInt and an AES of its own. This server keeps what
// the page hands it, in hm_pgpring (schema 6080, OpenPgpRing.h): the account's
// own keys - the secret half ENCRYPTED THE OPENPGP WAY under a passphrase that
// never leaves the browser, so this server holds it and cannot open it, and the
// same bytes saved to a file import into GnuPG - and the public keys of its
// correspondents, with where each came from.
//
// GET /api/v1/me/openpgp/keys lists both, with the limits and what lookup the
// administrator allows; PUT .../keys/own and PUT .../keys/correspondents add or
// replace one; DELETE .../keys/own/{fingerprint} and
// .../keys/correspondents/{fingerprint} remove one.
//
// THE FINGERPRINT IS NEVER THE CLIENT'S. Every row's fingerprint, key id,
// algorithm, creation and expiry are read out of the key itself by
// OpenPgpKey::Parse - the same reader the Web Key Directory publishes through -
// so a listing cannot say something about a key that the key does not say about
// itself, and a row cannot be filed under a fingerprint that is not its own.
//
// AND A SECRET KEY IN THE CLEAR IS REFUSED. OpenPgpKey::ReadSecretKey walks
// the block a PUT carries and requires every secret packet in it to be
// passphrase-encrypted, and requires its primary key to be the same key as the
// public half beside it. The encryption itself happens where this server cannot
// see it, which is exactly why this is the one place that can insist on it.
//
// GET /api/v1/me/openpgp/lookup?address= is the server-side half of finding a
// correspondent's key: a browser cannot make a cross-origin request to somebody
// else's domain and read the answer, so this server makes it - Web Key
// Directory first, keyserver after it and only if an administrator turned that
// on - through HttpsClient::RequestPublic, which refuses an address inside this
// network. Nothing is stored by the lookup: it answers the key and the page
// decides whether to keep it, because a key nobody looked at is not a key
// anybody trusts.
#include "StdAfx.h"
#include <mutex>
#include <set>

#include "RestApiServer.h"
#include "HttpServer.h"
#include "HttpCompression.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "OpenPgpRing.h"
#include "OpenPgpKey.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../BO/Account.h"
#include "../Util/Parsing/StringParser.h"

#include <string>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   extern const char *PortalHeaders;
   extern const char *const PortalOpenPgpScriptPieces[];

   namespace
   {
      AnsiString Utf8Of(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      String WideOf(const AnsiString &value)
      {
         String out;
         Unicode::MultiByteToWide(value, out);
         return out;
      }

      AnsiString EscapeJson(const AnsiString &value)
      {
         AnsiString out;
         for (int i = 0; i < value.GetLength(); i++)
         {
            unsigned char c = (unsigned char) value[i];
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else if (c < 0x20) { char buffer[8]; sprintf_s(buffer, sizeof(buffer), "\\u%04x", c); out += buffer; }
            else out += (char) c;
         }
         return out;
      }

      bool IsBase64Text(const std::string &text)
      {
         if (text.empty() || text.size() % 4 != 0)
            return false;
         for (size_t i = 0; i < text.size(); i++)
         {
            const char c = text[i];
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
            if (!ok)
               return false;
         }
         return true;
      }

      // The whole answer a route makes, so the routes below read as decisions
      // rather than as response building.
      struct Answer
      {
         Answer(int s, const AnsiString &b) : status(s), body(b) {}
         int status;
         AnsiString body;
      };

      Answer Refused(int status, const AnsiString &sentence)
      {
         return Answer(status, "{\"error\":\"" + EscapeJson(sentence) + "\"}");
      }

      // What every PUT carries, whichever kind it is: the public key, and what
      // the account calls whoever it belongs to. Everything the key says about
      // itself is read out of the key.
      bool ReadCommon(const JsonValue &document, OpenPgpRing::Row &row, AnsiString &problem)
      {
         const JsonValue *publicValue = document.Get("public_key");
         if (!publicValue || !publicValue->IsString())
         {
            problem = "public_key: the transferable public key, base64 of its bytes, is required";
            return false;
         }
         if (!IsBase64Text(publicValue->AsString()) || publicValue->AsString().size() > (size_t) OpenPgpRing::PublicMaximum)
         {
            problem = "public_key: base64 of the key's own bytes, at most 256 KB of base64";
            return false;
         }
         row.public_key = AnsiString(publicValue->AsString().c_str());

         const JsonValue *addressValue = document.Get("address");
         if (addressValue && addressValue->IsString())
         {
            Unicode::MultiByteToWide(AnsiString(addressValue->AsString().c_str()), row.address);
            row.address.ToLower();
            row.address.Trim();
         }

         const JsonValue *nameValue = document.Get("name");
         if (nameValue && nameValue->IsString())
            Unicode::MultiByteToWide(AnsiString(nameValue->AsString().c_str()), row.name);
         if (row.name.GetLength() > 255)
            row.name = row.name.Mid(0, 255);

         const JsonValue *preferValue = document.Get("prefer_encrypt");
         if (preferValue && preferValue->IsString())
         {
            const std::string prefer = preferValue->AsString();
            if (prefer != "mutual" && prefer != "nopreference")
            {
               problem = "prefer_encrypt: mutual or nopreference, as Autocrypt spells them";
               return false;
            }
            row.prefer = prefer == "mutual" ? 1 : 0;
         }

         const JsonValue *revokedValue = document.Get("revoked");
         if (revokedValue && revokedValue->IsBool())
            row.revoked = revokedValue->AsBool();

         // Everything else comes out of the key, not out of the request: the
         // fingerprint above all, which is what the row is filed under.
         if (!OpenPgpRing::DescribeFrom(row.public_key, row, problem))
            return false;

         // The name may now be the key's own user id, which is bounded only by
         // the key; the column is 255, and a value the column cannot take is a
         // 500 rather than a key stored.
         if (row.name.GetLength() > 255)
            row.name = row.name.Mid(0, 255);

         if (row.address.IsEmpty())
         {
            problem = "address: the key carries no address in its user ids, so one has to be given";
            return false;
         }
         if (!StringParser::IsValidEmailAddress(row.address) || row.address.GetLength() > 255)
         {
            problem = "address: not an e-mail address";
            return false;
         }

         return true;
      }

      // The bytes an account's ring holds, less the row about to be replaced.
      // Read through the listing the route itself uses, so the ceiling is the
      // ceiling on what that listing can become.
      // The accounts with a key lookup running now (HandleMeOpenPgpLookup_).
      std::mutex lookupsMutex;
      std::set<__int64> lookupsInFlight;

      __int64 HeldBytes(__int64 accountId, int replacedKind, const AnsiString &replacedFingerprint)
      {
         __int64 total = 0;
         const int kinds[2] = { OpenPgpRing::KindOwn, OpenPgpRing::KindCorrespondent };
         for (int k = 0; k < 2; k++)
         {
            std::vector<OpenPgpRing::Row> rows;
            if (!OpenPgpRing::List(accountId, kinds[k], rows))
               return -1;
            for (size_t i = 0; i < rows.size(); i++)
            {
               if (rows[i].kind == replacedKind && rows[i].fingerprint == replacedFingerprint)
                  continue;
               total += (__int64) rows[i].public_key.GetLength() + (__int64) rows[i].secret_key.GetLength();
            }
         }
         return total;
      }

      Answer SaveRow(std::shared_ptr<const Account> account, OpenPgpRing::Row &row)
      {
         OpenPgpRing::Row existing;
         const bool present = OpenPgpRing::Find(account->GetID(), row.kind, row.fingerprint, existing);

         const __int64 held = HeldBytes(account->GetID(), row.kind, row.fingerprint);
         if (held < 0)
            return Refused(500, "the keys already held could not be read");
         if (held + (__int64) row.public_key.GetLength() + (__int64) row.secret_key.GetLength() > OpenPgpRing::BytesPerAccount)
            return Refused(400, "the keys this account holds would exceed 8 MB together; remove one first");
         if (!present)
         {
            const int limit = row.kind == OpenPgpRing::KindOwn ? OpenPgpRing::OwnPerAccount : OpenPgpRing::CorrespondentsPerAccount;
            const int held = OpenPgpRing::Count(account->GetID(), row.kind);
            if (held < 0)
               return Refused(500, "the keys already held could not be counted");
            if (held >= limit)
            {
               AnsiString sentence;
               sentence.Format("at most %d %hs", limit, row.kind == OpenPgpRing::KindOwn ? "keys of your own" : "correspondents' keys");
               return Refused(400, sentence);
            }
         }

         bool replaced = false;
         if (!OpenPgpRing::Save(account->GetID(), row, replaced))
            return Refused(500, "the key could not be stored");

         OpenPgpRing::Row stored;
         if (!OpenPgpRing::Find(account->GetID(), row.kind, row.fingerprint, stored))
            return Refused(500, "the key could not be read back");

         return Answer(replaced ? 200 : 201, OpenPgpRing::RowJson(stored));
      }
   }

   // ---- routes --------------------------------------------------------------

   HttpResponse
   RestApiServer::HandleMeOpenPgpKeys_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      if (!Configuration::Instance()->GetOpenPGPPortalEnabled())
         return BuildResponse_(403, "{\"error\":\"OpenPGP is switched off in the webmail on this server\",\"code\":\"openpgp_unavailable\"}");

      std::vector<OpenPgpRing::Row> own;
      std::vector<OpenPgpRing::Row> correspondents;
      if (!OpenPgpRing::List(account->GetID(), OpenPgpRing::KindOwn, own) ||
          !OpenPgpRing::List(account->GetID(), OpenPgpRing::KindCorrespondent, correspondents))
         return BuildResponse_(500, "{\"error\":\"the keys could not be read\"}");

      AnsiString json = "{\"own\":[";
      for (size_t i = 0; i < own.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += OpenPgpRing::RowJson(own[i]);
      }
      json += "],\"correspondents\":[";
      for (size_t i = 0; i < correspondents.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += OpenPgpRing::RowJson(correspondents[i]);
      }

      AnsiString tail;
      tail.Format("],\"limits\":{\"own\":%d,\"correspondents\":%d},\"lookup\":{\"wkd\":%hs,\"keyserver\":%hs}}",
                  OpenPgpRing::OwnPerAccount, OpenPgpRing::CorrespondentsPerAccount,
                  Configuration::Instance()->GetOpenPGPLookupWkd() ? "true" : "false",
                  Configuration::Instance()->GetOpenPGPLookupKeyserver() ? "true" : "false");
      json += tail;
      return BuildResponse_(200, json);
   }

   HttpResponse
   RestApiServer::HandleMeOpenPgpKeyPut_(const Caller &caller, bool own, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      if (!Configuration::Instance()->GetOpenPGPPortalEnabled())
         return BuildResponse_(403, "{\"error\":\"OpenPGP is switched off in the webmail on this server\",\"code\":\"openpgp_unavailable\"}");

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.size()), document, parseError) || !document.IsObject())
         return BuildResponse_(400, "{\"error\":\"a JSON object is expected\"}");

      OpenPgpRing::Row row;
      row.kind = own ? OpenPgpRing::KindOwn : OpenPgpRing::KindCorrespondent;

      AnsiString problem;
      if (!ReadCommon(document, row, problem))
         return BuildResponse_(400, "{\"error\":\"" + EscapeJson(problem) + "\"}");

      const JsonValue *sourceValue = document.Get("source");
      String source = _T("imported");
      if (sourceValue && sourceValue->IsString())
         Unicode::MultiByteToWide(AnsiString(sourceValue->AsString().c_str()), source);
      if (!OpenPgpRing::IsSource(source))
         return BuildResponse_(400, "{\"error\":\"source: generated, imported, autocrypt, wkd or keyserver\"}");
      if (own && source != _T("generated") && source != _T("imported"))
         return BuildResponse_(400, "{\"error\":\"source: a key of your own was generated or imported; the other sources are a correspondent's\"}");
      row.source = source;

      if (own)
      {
         const JsonValue *secretValue = document.Get("secret_key");
         if (!secretValue || !secretValue->IsString() || secretValue->AsString().empty())
            return BuildResponse_(400, "{\"error\":\"secret_key: the armoured OpenPGP secret key, encrypted in the browser under its passphrase, is required for a key of your own\"}");
         if (secretValue->AsString().size() > (size_t) OpenPgpRing::SecretMaximum)
            return BuildResponse_(400, "{\"error\":\"secret_key: at most 512 KB of armour\"}");

         const AnsiString secret = AnsiString(secretValue->AsString().c_str());
         const OpenPgpKey::SecretKeyFacts facts = OpenPgpKey::ReadSecretKey(secret, (size_t) OpenPgpRing::SecretMaximum);
         if (!facts.ok)
            return BuildResponse_(400, "{\"error\":\"" + EscapeJson(facts.error) + "\"}");
         if (facts.fingerprint != row.fingerprint)
            return BuildResponse_(400, "{\"error\":\"the secret key and the public key are not the same key\"}");

         row.secret_key = secret;
      }
      else
      {
         const JsonValue *secretValue = document.Get("secret_key");
         if (secretValue && secretValue->IsString() && !secretValue->AsString().empty())
            return BuildResponse_(400, "{\"error\":\"secret_key: a correspondent's key is a public key; there is no secret half to store\"}");
      }

      Answer answer = SaveRow(account, row);
      return BuildResponse_(answer.status, answer.body);
   }

   HttpResponse
   RestApiServer::HandleMeOpenPgpKeyDelete_(const Caller &caller, bool own, const AnsiString &fingerprint)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      if (!Configuration::Instance()->GetOpenPGPPortalEnabled())
         return BuildResponse_(403, "{\"error\":\"OpenPGP is switched off in the webmail on this server\",\"code\":\"openpgp_unavailable\"}");

      AnsiString lowered = fingerprint;
      lowered.MakeLower();
      if (!OpenPgpRing::IsFingerprint(lowered))
         return BuildResponse_(404, "{\"error\":\"no such key\"}");

      if (!OpenPgpRing::Remove(account->GetID(), own ? OpenPgpRing::KindOwn : OpenPgpRing::KindCorrespondent, lowered))
         return BuildResponse_(404, "{\"error\":\"no such key\"}");

      return BuildResponse_(200, "{\"removed\":true}");
   }

   // GET /api/v1/me/openpgp/lookup?address=somebody@example.org
   HttpResponse
   RestApiServer::HandleMeOpenPgpLookup_(const Caller &caller, const AnsiString &query)
   {
      if (!caller.account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      if (!Configuration::Instance()->GetOpenPGPPortalEnabled())
         return BuildResponse_(403, "{\"error\":\"OpenPGP is switched off in the webmail on this server\",\"code\":\"openpgp_unavailable\"}");

      String address = WideOf(QueryParameter_(query, "address"));
      address.Trim();
      address.ToLower();
      if (address.IsEmpty() || !StringParser::IsValidEmailAddress(address) || address.GetLength() > 255)
         return BuildResponse_(400, "{\"error\":\"address: the e-mail address whose key to look for\"}");
      // The address validator admits a quoted local part, which may carry
      // characters a request line would read: nothing below 0x20 goes into a
      // URL this server will send.
      for (int i = 0; i < address.GetLength(); i++)
      {
         const wchar_t c = address.GetAt(i);
         if (c < 0x20 || c == 0x7f)
            return BuildResponse_(400, "{\"error\":\"address: not an e-mail address\"}");
      }

      if (!Configuration::Instance()->GetOpenPGPLookupWkd() && !Configuration::Instance()->GetOpenPGPLookupKeyserver())
         return BuildResponse_(403, "{\"error\":\"looking a key up is switched off on this server\",\"code\":\"lookup_unavailable\"}");

      // One lookup in flight per account. The route fetches from a host the
      // caller names, for up to the setting's seconds, on one of a handful of
      // REST workers; several at once from one mailbox would hold them all.
      {
         std::lock_guard<std::mutex> guard(lookupsMutex);
         if (lookupsInFlight.count(caller.account->GetID()))
            return BuildResponse_(429, "{\"error\":\"a key lookup for this account is already running; wait for it\",\"code\":\"lookup_busy\"}");
         lookupsInFlight.insert(caller.account->GetID());
      }
      struct Release
      {
         __int64 id;
         ~Release() { std::lock_guard<std::mutex> guard(lookupsMutex); lookupsInFlight.erase(id); }
      } release = { caller.account->GetID() };

      const OpenPgpRing::Found found = OpenPgpRing::Lookup(address);
      if (!found.Ok())
      {
         AnsiString json;
         json.Format("{\"found\":false,\"address\":\"%hs\",\"error\":\"%hs\",\"status\":%d}",
                     EscapeJson(Utf8Of(address)).c_str(), EscapeJson(Utf8Of(found.error)).c_str(), found.status);
         return BuildResponse_(200, json);
      }

      // Described from the key itself, and answered rather than stored: the
      // page shows the fingerprint and the person decides.
      OpenPgpRing::Row row;
      row.kind = OpenPgpRing::KindCorrespondent;
      row.public_key = found.key_base64;
      row.source = found.source.StartsWith(_T("wkd")) ? String(_T("wkd")) : String(_T("keyserver"));
      AnsiString problem;
      if (!OpenPgpRing::DescribeFrom(row.public_key, row, problem))
         return BuildResponse_(200, "{\"found\":false,\"error\":\"" + EscapeJson(problem) + "\"}");

      AnsiString json;
      json.Format("{\"found\":true,\"address\":\"%hs\",\"source\":\"%hs\",\"url\":\"%hs\",\"key\":",
                  EscapeJson(Utf8Of(address)).c_str(), EscapeJson(Utf8Of(found.source)).c_str(), EscapeJson(Utf8Of(found.url)).c_str());
      json += OpenPgpRing::RowJson(row);
      json += "}";
      return BuildResponse_(200, json);
   }

   // The page's OpenPGP module, joined from its pieces - and compressed - on
   // first use. It goes out unchanged on every request, so both are done once.
   HttpResponse
   RestApiServer::HandlePortalOpenPgpScript_()
   {
      static HttpStaticBody script;

      // The one switch, here as well as on the routes: an installation that has
      // turned OpenPGP off in the webmail is not serving a hundred kilobytes of
      // cryptography to anybody who asks for it.
      if (!Configuration::Instance()->GetOpenPGPPortalEnabled())
         return BuildResponse_(404, "{\"error\":\"not found\"}");

      HttpResponse response;
      response.content_type = "text/javascript; charset=utf-8";
      script.Fill(response, PortalOpenPgpScriptPieces);
      response.extra_headers = PortalHeaders;
      return response;
   }

   // The Autocrypt header a send may carry, checked before it is written into
   // a message. 1 when there is one and it is good, 0 when there is none, -1
   // when there is one and it is refused.
   //
   // The page builds it, because the page is where the key is: this server
   // never has to be able to make one, and never has to be trusted to. What it
   // does have to do is refuse a value that would put something other than one
   // header line into the message - a bare newline in a header value is header
   // injection, and this is a value a client chose. So: printable ASCII only,
   // every CR followed by LF and then the space or tab that makes it a FOLD
   // rather than a new header, an addr= to begin with, and a ceiling.
   int
   RestApiServer::AutocryptHeaderValue_(const AnsiString &requestBody, String &value, AnsiString &problem)
   {
      value = JsonUtf8Value_(requestBody, "autocrypt");
      if (value.IsEmpty())
         return 0;

      const AnsiString text = Utf8Of(value);
      if (text.GetLength() > 64 * 1024)
      {
         problem = "{\"error\":\"autocrypt: at most 64 KB\"}";
         return -1;
      }
      if (text.Mid(0, 5) != "addr=")
      {
         problem = "{\"error\":\"autocrypt: the header value, which begins addr=<address>\"}";
         return -1;
      }

      for (int i = 0; i < text.GetLength(); i++)
      {
         const unsigned char c = (unsigned char) text[i];
         if (c == '\r')
         {
            if (i + 2 >= text.GetLength() || text[i + 1] != '\n' || (text[i + 2] != ' ' && text[i + 2] != '\t'))
            {
               problem = "{\"error\":\"autocrypt: a line break in the value is a fold, so it is CRLF followed by a space or a tab; nothing else may break a line\"}";
               return -1;
            }
            i += 2;
            continue;
         }
         if (c < 0x20 || c > 0x7e)
         {
            problem = "{\"error\":\"autocrypt: printable ASCII only\"}";
            return -1;
         }
      }

      return 1;
   }

   AnsiString
   RestApiServer::OpenApiOpenPgpKeysPaths_()
   {
      return
         "\"/api/v1/me/openpgp/keys\":{\"get\":{\"summary\":\"The signed-in account's OpenPGP keys\",\"description\":\"own (the account's own keys, each with secret_key - the armoured OpenPGP secret key as the browser encrypted it, which this server stores and cannot open) and correspondents (public keys, with source: imported, autocrypt, wkd or keyserver, and first_seen and last_seen). Each row carries fingerprint, key_id, algorithm, created, expires, revoked and prefer_encrypt, every one of them read out of the key itself rather than taken from whoever stored it. limits says how many of each an account may hold; lookup says whether the administrator allows a Web Key Directory or a keyserver lookup. 403 with code openpgp_unavailable when OpenPGP is switched off in the webmail.\",\"responses\":{\"200\":{\"description\":\"own, correspondents, limits, lookup\"},\"403\":{\"description\":\"OpenPGP is switched off in the webmail\"}}}},"
         "\"/api/v1/me/openpgp/keys/own\":{\"put\":{\"summary\":\"Add or replace one of the signed-in account's own OpenPGP keys\",\"description\":\"Body: public_key (base64 of the transferable public key), secret_key (the armoured OpenPGP PRIVATE KEY BLOCK, encrypted in the browser under a passphrase this server never sees), address, name, source (generated or imported), prefer_encrypt (mutual or nopreference) and revoked. The secret key is REFUSED unless every secret packet in it is passphrase-encrypted and its primary key is the same key as public_key - a private key in the clear is not stored, and the two halves of a row are always one key. Filed under the fingerprint the key itself gives; 201 when it is new, 200 when it replaced one of the same fingerprint.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"public_key\",\"secret_key\"],\"properties\":{\"public_key\":{\"type\":\"string\"},\"secret_key\":{\"type\":\"string\"},\"address\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"The key as stored\"},\"201\":{\"description\":\"The key as stored\"},\"400\":{\"description\":\"Not a key, a secret key in the clear, the two halves not one key, or the account already holds twenty\"}}}},"
         "\"/api/v1/me/openpgp/keys/own/{fingerprint}\":{\"delete\":{\"summary\":\"Remove one of the signed-in account's own OpenPGP keys\",\"description\":\"The fingerprint as the listing prints it, lower-case hex. Removing a key here removes the only copy of it this server has: a key whose secret half is not written down elsewhere is gone, and the mail encrypted to it cannot be read again.\",\"responses\":{\"200\":{\"description\":\"removed\"},\"404\":{\"description\":\"No such key\"}}}},"
         "\"/api/v1/me/openpgp/keys/correspondents\":{\"put\":{\"summary\":\"Add or replace a correspondent's OpenPGP public key\",\"description\":\"Body: public_key (base64 of the transferable public key), address, name, source (imported, autocrypt, wkd or keyserver), prefer_encrypt and revoked. A secret key is refused here outright. first_seen is kept from the row that was there; last_seen is set to now, which is how a key learned from an Autocrypt header on every message stays current without the row being rewritten by hand.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"public_key\"],\"properties\":{\"public_key\":{\"type\":\"string\"},\"address\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"The key as stored\"},\"201\":{\"description\":\"The key as stored\"},\"400\":{\"description\":\"Not a public key, or the account already holds five hundred\"}}}},"
         "\"/api/v1/me/openpgp/keys/correspondents/{fingerprint}\":{\"delete\":{\"summary\":\"Forget a correspondent's OpenPGP public key\",\"responses\":{\"200\":{\"description\":\"removed\"},\"404\":{\"description\":\"No such key\"}}}},"
         "\"/api/v1/me/openpgp/lookup\":{\"get\":{\"summary\":\"Look a correspondent's OpenPGP key up\",\"description\":\"Query: address. This server fetches it, because a browser cannot make a cross-origin request to somebody else's domain and read the answer: the Web Key Directory of that domain first - openpgpkey.<domain> with the advanced path, then the domain itself with the direct one - and then, only where the administrator turned it on, the keyserver's HKP /vks/v1/by-email/. Every fetch is HTTPS, to a host that resolves only to public addresses, capped and deadlined. NOTHING IS STORED: the answer carries the key and where it came from, and the page decides whether to keep it with PUT .../keys/correspondents. found is false with a sentence in error when nothing was found, which is the ordinary case for an address whose owner publishes no key.\",\"responses\":{\"200\":{\"description\":\"found, address, source, url, key - or found false with error\"},\"400\":{\"description\":\"address missing or not an address\"},\"403\":{\"description\":\"OpenPGP or the lookup is switched off on this server\"}}}},";
   }
}
