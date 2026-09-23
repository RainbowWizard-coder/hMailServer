// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The signed-in account's masked addresses (MaskedAddresses.h):
//
//    GET    /api/v1/me/masked-addresses       every one, newest first
//    POST   /api/v1/me/masked-addresses       {label}: a new address, on
//    PUT    /api/v1/me/masked-addresses/{id}  {label, active}: renamed, or switched off and on
//    DELETE /api/v1/me/masked-addresses/{id}  gone, and mail to it refused

#include "StdAfx.h"
#include "RestApiServer.h"
#include "HttpServer.h"
#include "JsonDocument.h"
#include "MaskedAddresses.h"
#include "Unicode.h"
#include "../BO/Account.h"
#include "../Application/Configuration.h"
#include "../Application/EffectiveSetting.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      AnsiString MaskedJson(const MaskedAddresses::Entry &entry)
      {
         AnsiString json;
         json.Format("{\"id\":%I64d,\"active\":%hs,\"received\":%d,", entry.id, entry.active ? "true" : "false", entry.received);
         json += "\"address\":\"" + RestApiServer::JsonEscapeUtf8(entry.address) + "\"";
         json += ",\"label\":\"" + RestApiServer::JsonEscapeUtf8(entry.label) + "\"";
         json += ",\"created\":\"" + RestApiServer::JsonEscapeUtf8(entry.created) + "\"}";
         return json;
      }

      bool ReadLabel(const JsonValue &body, String &label, bool &given, AnsiString &problem)
      {
         const JsonValue *member = body.Get("label");
         if (!member || member->IsNull())
            return true;
         if (!member->IsString())
         {
            problem = "label must be a string";
            return false;
         }
         Unicode::MultiByteToWide(AnsiString(member->AsString().c_str()), label);
         label.Trim();
         for (int i = 0; i < label.GetLength(); i++)
         {
            if (label[i] < 0x20 || label[i] == 0x7f)
            {
               problem = "label may not contain control characters";
               return false;
            }
         }
         if (label.GetLength() > 255)
         {
            problem = "label is at most 255 characters";
            return false;
         }
         given = true;
         return true;
      }
   }

   AnsiString
   RestApiServer::OpenApiMaskedAddressesPaths_()
   {
      static const char *paths =
         ",\"/api/v1/me/masked-addresses\":{"
         "\"get\":{\"summary\":\"The signed-in account's masked addresses\",\"description\":\"{addresses:[{id,address,label,active,received,created}],max}, newest first. received counts the messages each has delivered. 403 when ReaderMaskedAddressesMax is 0.\",\"responses\":{\"200\":{\"description\":\"The addresses\"},\"403\":{\"description\":\"Not offered on this server\"}}},"
         "\"post\":{\"summary\":\"Make a masked address\",\"description\":\"Body: label (what it is for). The address is made in the account's own domain - a short form of the label, a dot and six random characters - and delivers to the mailbox at once. The account may write as it while it is on. 403 past ReaderMaskedAddressesMax.\",\"responses\":{\"201\":{\"description\":\"The address\"},\"400\":{\"description\":\"A label refused\"},\"403\":{\"description\":\"Not offered, or as many as allowed\"}}}},"
         "\"/api/v1/me/masked-addresses/{id}\":{"
         "\"put\":{\"summary\":\"Rename a masked address, or switch it off or on\",\"description\":\"Body: label, active - either. Switched off, mail to the address is refused at RCPT as mail to an unknown address is, and the account no longer writes as it.\",\"responses\":{\"200\":{\"description\":\"The address\"},\"404\":{\"description\":\"Not one of this account's\"}}},"
         "\"delete\":{\"summary\":\"Remove a masked address\",\"responses\":{\"200\":{\"description\":\"Removed\"},\"404\":{\"description\":\"Not one of this account's\"}}}}";
      return AnsiString(paths);
   }

   HttpResponse
   RestApiServer::HandleMeMaskedAddresses_(const Caller &caller, RouteKind kind, __int64 id, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // A setting with levels: the account's own ceiling, its domain's, or the
      // server's (docs/SettingLevels.md).
      const int ceiling = EffectiveSetting::ResolveInt(_T("reader_masked_addresses_max"), std::shared_ptr<const Domain>(), account);
      if (ceiling <= 0)
         return BuildResponse_(403, "{\"error\":\"masked addresses are not offered on this server\"}");

      if (kind == RouteMeMaskedAddresses)
      {
         std::vector<MaskedAddresses::Entry> entries;
         if (!MaskedAddresses::List(account->GetID(), entries))
            return BuildResponse_(500, "{\"error\":\"the addresses could not be read\"}");

         AnsiString body = "{\"addresses\":[";
         for (size_t i = 0; i < entries.size(); i++)
            body += (i ? "," : "") + MaskedJson(entries[i]);
         AnsiString tail;
         tail.Format("],\"max\":%d}", ceiling);
         return BuildResponse_(200, body + tail);
      }

      JsonValue body;
      if (kind == RouteMeMaskedAddressCreate || kind == RouteMeMaskedAddressUpdate)
      {
         std::string parseError;
         if (!JsonValue::Parse(std::string(requestBody.c_str(), (size_t) requestBody.GetLength()), body, parseError) || !body.IsObject())
            return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");
      }

      if (kind == RouteMeMaskedAddressCreate)
      {
         String label;
         bool given = false;
         AnsiString problem;
         if (!ReadLabel(body, label, given, problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

         if (MaskedAddresses::Count(account->GetID()) >= ceiling)
         {
            AnsiString refusal;
            refusal.Format("{\"error\":\"an account may have at most %d masked addresses\"}", ceiling);
            return BuildResponse_(403, refusal);
         }

         MaskedAddresses::Entry entry;
         String why;
         if (!MaskedAddresses::Create(account, label, entry, why))
            return BuildResponse_(500, "{\"error\":\"" + JsonEscape_(Utf8_(why)) + "\"}");

         LOG_APPLICATION("RestApi: " + account->GetAddress() + " made the masked address " + entry.address + ".");
         return BuildResponse_(201, MaskedJson(entry));
      }

      MaskedAddresses::Entry entry;
      if (!MaskedAddresses::Find(account->GetID(), id, entry))
         return BuildResponse_(404, "{\"error\":\"not one of your masked addresses\"}");

      if (kind == RouteMeMaskedAddressDelete)
      {
         if (!MaskedAddresses::Delete(account->GetID(), id))
            return BuildResponse_(500, "{\"error\":\"the address could not be removed\"}");
         LOG_APPLICATION("RestApi: " + account->GetAddress() + " removed the masked address " + entry.address + ".");
         return BuildResponse_(200, "{\"deleted\":true}");
      }

      if (kind == RouteMeMaskedAddressUpdate)
      {
         String label = entry.label;
         bool labelGiven = false;
         AnsiString problem;
         if (!ReadLabel(body, label, labelGiven, problem))
            return BuildResponse_(400, "{\"error\":\"" + JsonEscape_(problem) + "\"}");

         bool active = entry.active;
         if (const JsonValue *member = body.Get("active"))
         {
            if (!member->IsBool())
               return BuildResponse_(400, "{\"error\":\"active must be true or false\"}");
            active = member->AsBool();
         }

         for (const std::pair<std::string, JsonValue> &member : body.Members())
         {
            if (member.first != "label" && member.first != "active")
               return BuildResponse_(400, "{\"error\":\"a masked address takes label and active\"}");
         }

         if (!MaskedAddresses::Update(account->GetID(), id, label, active))
            return BuildResponse_(500, "{\"error\":\"the address could not be saved\"}");

         if (active != entry.active)
         {
            LOG_APPLICATION("RestApi: " + account->GetAddress() + (active ? " switched on " : " switched off ") + entry.address + ".");
         }

         entry.label = label;
         entry.active = active;
         return BuildResponse_(200, MaskedJson(entry));
      }

      return BuildResponse_(404, "{\"error\":\"not found\"}");
   }
}
