// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The signed-in account's addresses elsewhere, proved with a code and then sent
// as (SenderAddresses.h):
//
//    GET    /api/v1/me/sender-addresses              the addresses, proved or not
//    POST   /api/v1/me/sender-addresses              add one: the code is mailed to it
//    POST   /api/v1/me/sender-addresses/{id}/verify  the code, typed back
//    POST   /api/v1/me/sender-addresses/{id}/resend  a new code, no sooner than a minute on
//    DELETE /api/v1/me/sender-addresses/{id}         stop sending as it

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "JsonDocument.h"
#include "SenderAddresses.h"
#include "Unicode.h"
#include "Parsing/StringParser.h"
#include "../BO/Account.h"
#include "../Application/Configuration.h"
#include "../Cache/CacheContainer.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      AnsiString EntryJson(const SenderAddresses::Entry &entry, bool withPolicy)
      {
         AnsiString json;
         json.Format("{\"id\":%I64d,\"verified\":%hs,", entry.id, entry.verified ? "true" : "false");
         json += "\"address\":\"" + RestApiServer::JsonEscapeUtf8(entry.address) + "\"";
         json += ",\"name\":\"" + RestApiServer::JsonEscapeUtf8(entry.name) + "\"";
         json += ",\"created\":\"" + RestApiServer::JsonEscapeUtf8(entry.created) + "\"";
         if (withPolicy)
            json += ",\"dmarc_policy\":\"" + SenderAddresses::PolicyOf(entry.address) + "\"";
         return json + "}";
      }

      bool HasControlCharacters(const String &text)
      {
         for (int i = 0; i < text.GetLength(); i++)
         {
            if (text[i] < 0x20 || text[i] == 0x7f)
               return true;
         }
         return false;
      }
   }

   AnsiString
   RestApiServer::JsonEscapeUtf8(const String &value)
   {
      return JsonEscape_(Utf8_(value));
   }

   AnsiString
   RestApiServer::OpenApiSenderAddressesPaths_()
   {
      static const char *paths =
         ",\"/api/v1/me/sender-addresses\":{"
         "\"get\":{\"summary\":\"The addresses elsewhere the signed-in account sends as\",\"description\":\"{addresses:[{id,address,name,verified,created,dmarc_policy}],max}. dmarc_policy is what the address's domain publishes - reject, quarantine, none, or empty - because mail this server sends as the address is not aligned with that domain, and a receiver honouring reject or quarantine will not take it. 403 when ReaderSenderAddressesMax is 0.\",\"responses\":{\"200\":{\"description\":\"The addresses\"},\"403\":{\"description\":\"Not offered on this server\"}}},"
         "\"post\":{\"summary\":\"Add an address elsewhere to send as\",\"description\":\"Body: address, and name (optional). A six-digit code is mailed to the address from the postmaster of the account's domain; the address is sent as only once POST .../verify has the code. An address of a domain on this server is refused - an alias or the post right on the owner's inbox is how one is shared - as is one the account can already write as, one it has added already, and one past ReaderSenderAddressesMax.\",\"responses\":{\"201\":{\"description\":\"The address, not yet verified\"},\"400\":{\"description\":\"Not an address, or one on this server\"},\"403\":{\"description\":\"Not offered, or as many as allowed\"},\"409\":{\"description\":\"Already one the account writes as or has added\"}}}},"
         "\"/api/v1/me/sender-addresses/{id}\":{\"delete\":{\"summary\":\"Stop sending as an address elsewhere\",\"responses\":{\"200\":{\"description\":\"Removed\"},\"404\":{\"description\":\"Not one of this account's\"}}}},"
         "\"/api/v1/me/sender-addresses/{id}/verify\":{\"post\":{\"summary\":\"Prove an address with the code mailed to it\",\"description\":\"Body: code. A code lasts an hour and five wrong tries end it.\",\"responses\":{\"200\":{\"description\":\"The address, verified\"},\"400\":{\"description\":\"Not the code; attempts_left says how many tries remain\"},\"404\":{\"description\":\"Not one of this account's\"},\"410\":{\"description\":\"No code is waiting: it expired, the server restarted, or too many were wrong - send a new one\"}}}},"
         "\"/api/v1/me/sender-addresses/{id}/resend\":{\"post\":{\"summary\":\"Mail a new code\",\"responses\":{\"202\":{\"description\":\"Sent\"},\"404\":{\"description\":\"Not one of this account's\"},\"409\":{\"description\":\"Already verified\"},\"429\":{\"description\":\"The last code went out less than a minute ago; retry_after says when\"}}}}";
      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleMeSenderAddresses_(const Caller &caller, RouteKind kind, __int64 id, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      const int ceiling = Configuration::Instance()->GetReaderSenderAddressesMax();
      if (ceiling <= 0)
         return BuildResponse_(403, "{\"error\":\"sending as an address elsewhere is not offered on this server\"}");

      if (kind == RouteMeSenderAddresses)
      {
         std::vector<SenderAddresses::Entry> entries;
         if (!SenderAddresses::List(account->GetID(), entries))
            return BuildResponse_(500, "{\"error\":\"the addresses could not be read\"}");

         AnsiString body = "{\"addresses\":[";
         for (size_t i = 0; i < entries.size(); i++)
            body += (i ? "," : "") + EntryJson(entries[i], true);
         AnsiString tail;
         tail.Format("],\"max\":%d}", ceiling);
         return BuildResponse_(200, body + tail);
      }

      if (kind == RouteMeSenderAddressCreate)
      {
         JsonValue body;
         std::string parseError;
         if (!JsonValue::Parse(std::string(requestBody.c_str(), (size_t) requestBody.GetLength()), body, parseError) || !body.IsObject())
            return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

         String address;
         String name;
         Unicode::MultiByteToWide(AnsiString(body.GetString("address").c_str()), address);
         Unicode::MultiByteToWide(AnsiString(body.GetString("name").c_str()), name);
         address.Trim();
         address.ToLower();
         name.Trim();

         if (address.IsEmpty() || address.GetLength() > 255 || name.GetLength() > 255 ||
             HasControlCharacters(address) || HasControlCharacters(name) || !StringParser::IsValidEmailAddress(address))
            return BuildResponse_(400, "{\"error\":\"address must be one e-mail address, and name at most 255 characters\"}");

         if (CacheContainer::Instance()->GetDomain(StringParser::ExtractDomain(address)))
            return BuildResponse_(400, "{\"error\":\"that address is on this server: its owner shares it with an alias or the post right on their inbox\"}");

         std::vector<Identity> identities;
         IdentitiesFor_(account, identities);
         for (const Identity &identity : identities)
         {
            if (identity.address.CompareNoCase(address) == 0)
               return BuildResponse_(409, "{\"error\":\"you can already write as that address\"}");
         }

         std::vector<SenderAddresses::Entry> entries;
         SenderAddresses::List(account->GetID(), entries);
         for (const SenderAddresses::Entry &entry : entries)
         {
            if (entry.address.CompareNoCase(address) == 0)
               return BuildResponse_(409, "{\"error\":\"you have added that address already\"}");
         }
         if ((int) entries.size() >= ceiling)
         {
            AnsiString refusal;
            refusal.Format("{\"error\":\"an account may send as at most %d addresses elsewhere\"}", ceiling);
            return BuildResponse_(403, refusal);
         }

         __int64 made = 0;
         if (!SenderAddresses::Add(account->GetID(), address, name, made))
            return BuildResponse_(500, "{\"error\":\"the address could not be saved\"}");

         SenderAddresses::Entry entry;
         SenderAddresses::Find(account->GetID(), made, entry);

         int wait = 0;
         const AnsiString code = SenderAddresses::NewCode(made, wait);
         bool sent = !code.IsEmpty() && SenderAddresses::SendCode(account, entry, code);

         LOG_APPLICATION("RestApi: " + account->GetAddress() + " added " + address + " to send as.");

         AnsiString json = EntryJson(entry, true);
         json = json.Mid(0, json.GetLength() - 1) + (sent ? ",\"code_sent\":true}" : ",\"code_sent\":false}");
         return BuildResponse_(201, json);
      }

      SenderAddresses::Entry entry;
      if (!SenderAddresses::Find(account->GetID(), id, entry))
         return BuildResponse_(404, "{\"error\":\"not one of your addresses\"}");

      if (kind == RouteMeSenderAddressDelete)
      {
         if (!SenderAddresses::Delete(account->GetID(), id))
            return BuildResponse_(500, "{\"error\":\"the address could not be removed\"}");
         LOG_APPLICATION("RestApi: " + account->GetAddress() + " no longer sends as " + entry.address + ".");
         return BuildResponse_(200, "{\"deleted\":true}");
      }

      if (kind == RouteMeSenderAddressResend)
      {
         if (entry.verified)
            return BuildResponse_(409, "{\"error\":\"that address is verified already\"}");

         int wait = 0;
         const AnsiString code = SenderAddresses::NewCode(id, wait);
         if (code.IsEmpty())
         {
            AnsiString refusal;
            refusal.Format("{\"error\":\"a code went out less than a minute ago\",\"retry_after\":%d}", wait);
            AnsiString header;
            header.Format("Retry-After: %d\r\n", wait > 0 ? wait : 60);
            return BuildResponse_(429, refusal, header);
         }
         if (!SenderAddresses::SendCode(account, entry, code))
            return BuildResponse_(500, "{\"error\":\"the code could not be sent; see the error log\"}");
         return BuildResponse_(202, "{\"code_sent\":true}");
      }

      if (kind == RouteMeSenderAddressVerify)
      {
         if (entry.verified)
            return BuildResponse_(200, EntryJson(entry, false));

         const AnsiString code = GetJsonStringValue_(requestBody, "code");
         int attemptsLeft = 0;
         switch (SenderAddresses::CheckCode(id, code, attemptsLeft))
         {
         case SenderAddresses::CodeRight:
            if (!SenderAddresses::MarkVerified(account->GetID(), id))
               return BuildResponse_(500, "{\"error\":\"the address could not be saved as verified\"}");
            entry.verified = true;
            LOG_APPLICATION("RestApi: " + account->GetAddress() + " proved " + entry.address + " and may send as it.");
            return BuildResponse_(200, EntryJson(entry, false));
         case SenderAddresses::CodeWrong:
         {
            AnsiString refusal;
            refusal.Format("{\"error\":\"that is not the code\",\"attempts_left\":%d}", attemptsLeft);
            return BuildResponse_(400, refusal);
         }
         case SenderAddresses::CodeLocked:
            return BuildResponse_(410, "{\"error\":\"too many wrong codes: send a new one\"}");
         case SenderAddresses::CodeExpired:
         default:
            return BuildResponse_(410, "{\"error\":\"no code is waiting for that address: send a new one\"}");
         }
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }
}
