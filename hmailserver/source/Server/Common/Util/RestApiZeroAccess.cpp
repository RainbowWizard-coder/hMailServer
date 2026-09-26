// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Zero-access mailboxes over the self-service API (Common/Util/ZeroAccess.h,
// docs/ZeroAccess.md): the account's own switch, and the two wrapped copies
// of its key that the switch stands on.
//
//   GET    /api/v1/me/zero-access          allowed (the server-wide switch),
//                                          enabled, and when enabled the row:
//                                          the key's fingerprint, the Argon2id
//                                          salt and parameters, both wrapped
//                                          copies, when it was switched on.
//   PUT    /api/v1/me/zero-access          switch on: fingerprint of one of
//                                          the account's own ring keys, salt,
//                                          passes, memory_kib, lanes,
//                                          password_wrapped, recovery_wrapped.
//   PUT    /api/v1/me/zero-access/rewrap   a new password-wrapped copy under
//                                          new parameters: a password change
//                                          through the page (which sends it
//                                          with the change - HandleMePassword_
//                                          calls the same code), or a recovery
//                                          key unlock followed by a re-wrap.
//   DELETE /api/v1/me/zero-access          switch off. Decrypts nothing.
//
// EVERYTHING HERE IS THE PAGE'S WORD FOR WHAT THE COPIES ARE. This server
// checks that each wrapped copy is an OpenPGP secret key of the fingerprint
// named with every secret packet encrypted, that the key is one it can
// encrypt to, and that the parameters are within bounds - and cannot check
// that the password-derived passphrase or the recovery key actually opens
// them, because it has neither. A page that stored copies it could not open
// would find out at the next sign-in, which is why the switch-on flow
// (Portal.js) types the recovery key back before it stores anything.
//
// The three writes are refused to a request that came in on an app password
// and to an administrator's support session (RefuseUnlessAccountHolder_):
// either can read the row - without the wrapped copies (WithholdsWrappedKeys_)
// - but the switch and the copies are the person's own. A support session
// that could switch zero access off would store the next delivery in the
// clear for the administrator the switch exists to keep out, and one that
// could re-wrap would replace the copy the person opens their mailbox with.
#include "StdAfx.h"

#include "RestApiServer.h"
#include "HttpServer.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "ZeroAccess.h"
#include "OpenPgpRing.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../Application/EffectiveSetting.h"
#include "../Application/Logger.h"
#include "../BO/Account.h"
#include "../Cache/CacheContainer.h"

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

      AnsiString StringOf(const JsonValue &document, const char *name)
      {
         const JsonValue *value = document.Get(name);
         if (!value || !value->IsString())
            return AnsiString();
         return AnsiString(value->AsString().c_str());
      }

      int NumberOf(const JsonValue &document, const char *name)
      {
         const JsonValue *value = document.Get(name);
         if (!value || !value->IsNumber())
            return -1;
         return (int) value->AsNumber();
      }

      // What every write carries: the derivation and the password-wrapped
      // copy; the switch-on carries the key and the recovery copy as well.
      void ReadRow(const JsonValue &document, ZeroAccess::Row &row)
      {
         row.fingerprint = StringOf(document, "fingerprint");
         row.fingerprint.MakeLower();
         row.salt = StringOf(document, "salt");
         row.passes = NumberOf(document, "passes");
         row.memory_kib = NumberOf(document, "memory_kib");
         row.lanes = NumberOf(document, "lanes");
         row.password_wrapped = StringOf(document, "password_wrapped");
         row.recovery_wrapped = StringOf(document, "recovery_wrapped");
      }
   }

   AnsiString
   RestApiServer::ZeroAccessJson_(std::shared_ptr<const Account> account, bool keysIncluded)
   {
      AnsiString json = "{\"allowed\":";
      json += ZeroAccess::Allowed(account) ? "true" : "false";
      ZeroAccess::Row row;
      if (account->GetZeroAccess())
      {
         if (ZeroAccess::Get(account->GetID(), row))
         {
            // The two wrapped copies are the mailbox key sealed under the
            // account password (by Argon2id with the salt and parameters
            // beside it) and under the recovery key: each is an offline test
            // of a guess at what it is sealed under. Only the page the account
            // holder signed in to with the password needs them.
            if (!keysIncluded)
            {
               AnsiString withheld;
               withheld.Format(",\"enabled\":true,\"fingerprint\":\"%hs\",\"enabled_at\":%I64d,\"keys_withheld\":true,\"note\":\"%hs\"}",
                               row.fingerprint.c_str(), row.enabled_at, WrappedKeysWithheld_());
               json += withheld;
               return json;
            }

            AnsiString body = ZeroAccess::RowJson(row);
            // RowJson opens with {"enabled":true, and this document already
            // has its brace: join after the brace.
            json += "," + body.Mid(1);
            return json;
         }
         // The switch is on and the row is gone - a restore that dropped the
         // table, a database edited by hand - which is the state SealFile
         // refuses every delivery in. Said so, rather than "off" beside a
         // switch-on form that would be answered 409: the way out is DELETE,
         // and the page offers it for this answer.
         json += ",\"enabled\":true,\"broken\":true,\"problem\":\"zero access is on for this mailbox and its key row is missing, so every delivery to it is being refused: switch zero access off and on again\"}";
         return json;
      }
      json += ",\"enabled\":false}";
      return json;
   }

   HttpResponse
   RestApiServer::HandleMeZeroAccess_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      return BuildResponse_(200, ZeroAccessJson_(account, !WithholdsWrappedKeys_(caller)));
   }

   HttpResponse
   RestApiServer::HandleMeZeroAccessPut_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      if (!ZeroAccess::Allowed(account))
         return BuildResponse_(403, "{\"error\":\"zero-access mailboxes are not allowed on this server; an administrator turns them on with the ZeroAccessAllowed setting\",\"code\":\"zero_access_not_allowed\"}");

      if (!EffectiveSetting::ResolveBool(_T("openpgp_portal_enabled"), std::shared_ptr<const Domain>(), account))
         return BuildResponse_(403, "{\"error\":\"OpenPGP is switched off in the webmail for this account\",\"code\":\"openpgp_unavailable\"}");

      HttpResponse refusal;
      if (RefuseUnlessAccountHolder_(caller, "switching the mailbox to zero access", refusal))
         return refusal;

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.size()), document, parseError) || !document.IsObject())
         return BuildResponse_(400, "{\"error\":\"a JSON object is expected\"}");

      ZeroAccess::Row row;
      ReadRow(document, row);

      AnsiString problem;
      int status = 500;
      if (!ZeroAccess::Enable(account, row, problem, status))
         return BuildResponse_(status, "{\"error\":\"" + EscapeJson(problem) + "\"}");

      LOG_APPLICATION("REST API: " + account->GetAddress() + " switched its mailbox to zero access from " + String(caller.peer.ToString()) + ".");

      // The account object the caller holds is the cache's from before the
      // flag was written; the row is read back so the answer is the truth.
      std::shared_ptr<const Account> fresh = CacheContainer::Instance()->GetAccount(account->GetID());
      return BuildResponse_(201, ZeroAccessJson_(fresh ? fresh : account, !WithholdsWrappedKeys_(caller)));
   }

   HttpResponse
   RestApiServer::HandleMeZeroAccessRewrap_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      HttpResponse refusal;
      if (RefuseUnlessAccountHolder_(caller, "re-wrapping the mailbox key", refusal))
         return refusal;

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.size()), document, parseError) || !document.IsObject())
         return BuildResponse_(400, "{\"error\":\"a JSON object is expected\"}");

      ZeroAccess::Row row;
      ReadRow(document, row);

      AnsiString problem;
      int status = 500;
      if (!ZeroAccess::Rewrap(account, row, problem, status))
         return BuildResponse_(status, "{\"error\":\"" + EscapeJson(problem) + "\"}");

      LOG_APPLICATION("REST API: " + account->GetAddress() + " re-wrapped its zero-access mailbox key from " + String(caller.peer.ToString()) + ".");
      return BuildResponse_(200, ZeroAccessJson_(account, !WithholdsWrappedKeys_(caller)));
   }

   HttpResponse
   RestApiServer::HandleMeZeroAccessDelete_(const Caller &caller)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      HttpResponse refusal;
      if (RefuseUnlessAccountHolder_(caller, "switching the mailbox off zero access", refusal))
         return refusal;

      AnsiString problem;
      if (!ZeroAccess::Disable(account, problem))
      {
         const bool absent = problem.Find("not on") >= 0;
         return BuildResponse_(absent ? 404 : 500, "{\"error\":\"" + EscapeJson(problem) + "\"}");
      }

      LOG_APPLICATION("REST API: " + account->GetAddress() + " switched its mailbox off zero access from " + String(caller.peer.ToString()) + ".");
      return BuildResponse_(200, "{\"enabled\":false,\"note\":\"what was stored encrypted stays encrypted; new mail is stored as it arrives\"}");
   }

   // The password change carries the re-wrapped copy, or is refused: a
   // zero-access mailbox's key is sealed under the password, and a password
   // changed without the copy sealed again under it is a mailbox that opens
   // only with the recovery key from then on. Called by HandleMePassword_
   // before the password is written, with the new copy's fields read from the
   // same body; answers the status to refuse with, 0 when the change may go
   // ahead. rewrapped is the row to write once the password has been saved.
   int
   RestApiServer::ZeroAccessPasswordCheck_(std::shared_ptr<const Account> account, const AnsiString &requestBody, ZeroAccess::Row &rewrapped, AnsiString &problem)
   {
      if (!account->GetZeroAccess())
         return 0;

      JsonValue document;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), requestBody.size()), document, parseError) || !document.IsObject())
      {
         problem = "a JSON object is expected";
         return 400;
      }

      const JsonValue *zero = document.Get("zero_access");
      if (!zero || !zero->IsObject())
      {
         problem = "this mailbox is a zero-access mailbox: its key is sealed under the password, so a password change carries the key sealed again under the new one (zero_access: salt, passes, memory_kib, lanes, password_wrapped). Change the password from the webmail, which does this.";
         return 409;
      }

      ReadRow(*zero, rewrapped);
      rewrapped.recovery_wrapped.Empty();

      ZeroAccess::Row existing;
      if (!ZeroAccess::Get(account->GetID(), existing))
      {
         problem = "this mailbox has zero access on and no key row; switch zero access off and on again";
         return 409;
      }
      rewrapped.fingerprint = existing.fingerprint;

      int status = 400;
      if (!ZeroAccess::Validate(account->GetID(), rewrapped, false, problem, status))
      {
         problem = "zero_access: " + problem;
         return status;
      }
      return 0;
   }

   // The administrator's read of a zero-access mailbox's mail, refused by
   // name: the message source, and the export that packages it. The headers
   // are answered by the routes that answer headers, because the headers
   // were kept in the clear and the document says so.
   bool
   RestApiServer::RefuseZeroAccessRead_(std::shared_ptr<const Account> account, HttpResponse &refusal)
   {
      if (!account || !account->GetZeroAccess())
         return false;

      refusal = BuildResponse_(403, "{\"error\":\"this is a zero-access mailbox: its mail is encrypted to a key this server cannot open, and the administrator's read of it is refused by design (docs/ZeroAccess.md). The headers are the part the server kept.\",\"code\":\"zero_access\"}");
      return true;
   }

   AnsiString
   RestApiServer::OpenApiZeroAccessPaths_()
   {
      return
         ",\"/api/v1/me/zero-access\":{\"get\":{\"summary\":\"Whether the signed-in account is a zero-access mailbox, and the key it stands on\",\"description\":\"allowed says whether the administrator permits the opt-in (the ZeroAccessAllowed setting); enabled whether this mailbox has made it. When enabled: fingerprint (the ring key every delivery is encrypted to), salt and argon2 (passes, memory_kib, lanes - what the browser derives the key-encryption passphrase from the account password with, RFC 9106), password_wrapped (the armoured OpenPGP secret key under that passphrase), recovery_wrapped (the same key under the recovery key the person wrote down) and enabled_at. This server holds both copies and can open neither. A request made with an app password, or in an administrator's support session, is answered fingerprint, enabled_at, keys_withheld true and a note, without salt, argon2 or either copy: each copy is an offline test of a guess at what it is sealed under. When the switch is on and the key row is missing (a restore that dropped the table, a database edited by hand), enabled is true with broken true and a sentence in problem, and every delivery is being refused until DELETE switches it off.\",\"responses\":{\"200\":{\"description\":\"allowed, enabled, and the row when enabled\"}}},"
         "\"put\":{\"summary\":\"Switch the signed-in account to a zero-access mailbox\",\"description\":\"Body: fingerprint (one of the account's own ring keys, not revoked, one that does not expire, one this server can encrypt to - RSA or Curve25519; a key that expires is refused with a sentence, because every delivery would be refused from the day it did), salt (base64 of 16 octets), passes, memory_kib and lanes (the Argon2id parameters, within bounds), password_wrapped and recovery_wrapped (the armoured secret key sealed in the browser under the password-derived passphrase and under the recovery key). Each copy is checked to be an encrypted secret key of that fingerprint; nothing here can check that either opens, which is why the page types the recovery key back before it sends this. From the moment this answers 201 every message delivered to the account is encrypted to the key after the filters have run, the administrator's read and export of the mailbox answer 403, search sees the headers only, IMAP and POP hand out the ciphertext, and a forgotten password without the recovery key is the mail lost. 403 with code zero_access_not_allowed while the administrator has not allowed it; 403 on an app password or in an administrator's support session; 409 while it is already on.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"fingerprint\",\"salt\",\"passes\",\"memory_kib\",\"lanes\",\"password_wrapped\",\"recovery_wrapped\"],\"properties\":{\"fingerprint\":{\"type\":\"string\"},\"salt\":{\"type\":\"string\"},\"passes\":{\"type\":\"integer\"},\"memory_kib\":{\"type\":\"integer\"},\"lanes\":{\"type\":\"integer\"},\"password_wrapped\":{\"type\":\"string\"},\"recovery_wrapped\":{\"type\":\"string\"}}}}}},\"responses\":{\"201\":{\"description\":\"The row as stored\"},\"400\":{\"description\":\"A copy that is not an encrypted secret key of the fingerprint, a key this server cannot encrypt to, or parameters out of bounds\"},\"403\":{\"description\":\"Not allowed on this server, OpenPGP switched off in the webmail, or an app password or a support session\"},\"404\":{\"description\":\"No key of the account's own with that fingerprint\"},\"409\":{\"description\":\"Already on\"}}},"
         "\"delete\":{\"summary\":\"Switch the signed-in account back to an ordinary mailbox\",\"description\":\"Clears the switch and removes the wrapped copies. DECRYPTS NOTHING: every message stored while the switch was on stays encrypted to the key, which remains in the ring and opens in the webmail with its own passphrase. New mail is stored as it arrives. 403 on an app password or in an administrator's support session; 404 when it was not on.\",\"responses\":{\"200\":{\"description\":\"enabled false\"},\"403\":{\"description\":\"An app password or a support session\"},\"404\":{\"description\":\"Not on\"}}}},"
         "\"/api/v1/me/zero-access/rewrap\":{\"put\":{\"summary\":\"Seal the mailbox key again under a new password-derived passphrase\",\"description\":\"Body: salt, passes, memory_kib, lanes and password_wrapped, as PUT /api/v1/me/zero-access takes them. The key stays the key and the recovery copy stays as it is; what changes is what the password copy is sealed under. This is what the page sends after the person unlocked the key with the recovery key because an administrator or a reset link had set a password the page never saw - and what a password change from the page carries inside POST /api/v1/me/password, which refuses a change without it for a zero-access mailbox. 403 on an app password or in an administrator's support session; 404 when zero access is not on.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"salt\",\"passes\",\"memory_kib\",\"lanes\",\"password_wrapped\"],\"properties\":{\"salt\":{\"type\":\"string\"},\"passes\":{\"type\":\"integer\"},\"memory_kib\":{\"type\":\"integer\"},\"lanes\":{\"type\":\"integer\"},\"password_wrapped\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"The row as stored\"},\"400\":{\"description\":\"Not an encrypted secret key of the mailbox's key, or parameters out of bounds\"},\"403\":{\"description\":\"An app password or a support session\"},\"404\":{\"description\":\"Zero access is not on\"}}}}";
   }
}
