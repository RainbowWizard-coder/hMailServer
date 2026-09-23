// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The REST API's setting profiles. See RestApiServer.h, Common/Application/Profiles.h
// and docs/Profiles.md.
//
//    GET  /api/v1/settings/profiles               the four, and what is applied
//    GET  /api/v1/settings/profiles/{name}        the diff: every entry beside the
//                                                 value this server holds now
//    POST /api/v1/settings/profiles/{name}/apply  the snapshot, then the writes;
//                                                 {"confirm": true} for an entry that
//                                                 closes a listener, trusts an address
//                                                 range or turns a filter off
//    POST /api/v1/settings/profiles/revert        the way back, and it is cleared
//    POST /api/v1/settings/profiles/discard       the way back forgotten, the
//                                                 settings kept
//
// The routes are the server's rather than a domain's, so a credential confined to
// named domains is refused them. Reading needs view_configuration, as reading the
// settings does; the three that write need manage_settings, as a settings PUT does.
// An apply and a revert are on the audit trail as one event each, with the count,
// and every setting they write as the row the settings chokepoint writes for it.
//
// The answers, for the three that write:
//    200  done; the body says what each entry did
//    404  no profile of that name
//    409  refused, and "error" says why - a way back is held, there is none, an entry
//         needs the confirmation (confirmation true, and confirmation_needed lists
//         them), or an entry would be refused by its setting (refused lists them)
//    500  something failed part way; the body says what was written
#include "StdAfx.h"
#include "RestApiServer.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "../Application/Profiles.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   HttpResponse
   RestApiServer::HandleSettingsProfiles_(const Route &route, const AnsiString &requestBody)
   {
      if (route.kind == RouteSettingsProfileList)
         return BuildResponse_(200, Profiles::ListJson());

      String name;
      Unicode::MultiByteToWide(route.identifier, name);

      if (route.kind == RouteSettingsProfileGet)
      {
         AnsiString json;
         if (!Profiles::DiffJson(name, json))
            return BuildResponse_(404, "{\"error\":\"" + JsonEscape_(Utf8_(Formatter::Format("There is no profile named {0}. The profiles are office, gateway, relay and sender.", name))) + "\"}");
         return BuildResponse_(200, json);
      }

      AnsiString json;
      String sentence;
      Profiles::Outcome outcome = Profiles::OutcomeFailed;

      if (route.kind == RouteSettingsProfileApply)
      {
         // The body is optional: an apply with nothing to confirm needs none. When
         // there is one it is an object, and confirm, when present, is a boolean -
         // a confirmation given by accident, as a string, is not one.
         bool confirmed = false;
         std::string body(requestBody.c_str(), requestBody.GetLength());
         if (body.find_first_not_of(" \t\r\n") != std::string::npos)
         {
            JsonValue document;
            std::string parseError;
            if (!JsonValue::Parse(body, document, parseError) || !document.IsObject())
               return BuildResponse_(400, "{\"error\":\"the body must be a JSON object, such as {\\\"confirm\\\": true}\"}");

            const JsonValue *confirm = document.Get("confirm");
            if (confirm && !confirm->IsNull())
            {
               if (!confirm->IsBool())
                  return BuildResponse_(400, "{\"error\":\"confirm must be true or false\"}");
               confirmed = confirm->AsBool();
            }
         }

         outcome = Profiles::Apply(name, confirmed, json, sentence);
      }
      else if (route.kind == RouteSettingsProfileRevert)
         outcome = Profiles::Revert(json, sentence);
      else
         outcome = Profiles::Discard(json, sentence);

      switch (outcome)
      {
      case Profiles::OutcomeDone:
         return BuildResponse_(200, json);
      case Profiles::OutcomeUnknown:
         return BuildResponse_(404, "{\"error\":\"" + JsonEscape_(Utf8_(sentence)) + "\"}");
      default:
         break;
      }

      // A refusal and a failure carry the sentence and whatever the server can say
      // about the state it is in, so that a page can redraw from the answer it has.
      AnsiString body = "{\"error\":\"" + JsonEscape_(Utf8_(sentence)) + "\"";
      if (!json.IsEmpty() && json[0] == '{' && json.GetLength() > 2)
         body += "," + json.Mid(1);
      else
         body += "}";

      return BuildResponse_(outcome == Profiles::OutcomeRefused ? 409 : 500, body);
   }

   AnsiString
   RestApiServer::OpenApiProfilesPaths_()
   {
      static const char *paths =
         ",\"/api/v1/settings/profiles\":{\"get\":{\"summary\":\"The setting profiles, and the one applied\","
         "\"description\":\"The four profiles this server ships (docs/Profiles.md): office (a small office server), gateway (behind a filtering gateway), relay (an outbound relay) and sender (a high-volume sender). Each is {name, title, summary, includes, entries, changes, confirmation_needed, skipped, refused}: how many settings it sets, how many of them applying it here would change, how many of those close a listener, trust an address range or turn a filter off and so ask for a confirmation, how many the server's environment sets and are skipped, and how many a setting would refuse. applied is the last profile applied - {profile, title, applied, state, changed}, state applied, reverted or kept - or null; snapshot is the way back from it while one is held - {readable, profile, applied, entries} - or null. A profile is a document in the tree and none is added here. Server-wide; refused for domain-restricted keys.\","
         "\"responses\":{\"200\":{\"description\":\"{profiles: [...], applied, snapshot}\"}}}},"
         "\"/api/v1/settings/profiles/{name}\":{\"get\":{\"summary\":\"One profile's diff against this server\","
         "\"description\":\"Every entry of the profile beside the value this server holds now: {store, name, value, current, stored, changes, why, caution, confirm, route, applies, levels, environment, refused}. store is ini for a [Settings] key and database for an hm_settings property; current is the value in force - the stored one, or the shipped default where none is stored - and changes whether applying would move it. route is the REST settings route and key the value is written through, the same setter PUT on that route runs. applies is when a change reaches the running server: now, reinitialize or restart. levels is limit, gate or preference for a setting a domain and an account may hold their own value of (docs/SettingLevels.md) - the profile sets the server's level, which for a limit is the ceiling the levels below it are held to. caution is closes_listener, trusts_addresses or turns_filter_off, and confirm is true where such an entry would change something here, which an apply refuses without the confirmation. environment names the variable where the server's environment sets the setting, which the apply leaves as it is; refused carries the sentence the setting's own check refuses the value with. steps is what the profile cannot do for the administrator. applicable says whether an apply would be taken now, and blocked why not. 404 for a name that is no profile. Server-wide; refused for domain-restricted keys.\","
         "\"responses\":{\"200\":{\"description\":\"{name, title, summary, includes, steps, entries: [...], changes, unchanged, skipped, refused, confirmation_needed: [names], applicable, blocked, applied, snapshot}\"},\"404\":{\"description\":\"No such profile\"}}}},"
         "\"/api/v1/settings/profiles/{name}/apply\":{\"post\":{\"summary\":\"Apply a profile\","
         "\"description\":\"Body, optional: {confirm: true}. Every entry is checked first, as the diff shows it, and the apply is all or nothing: an entry its setting would refuse refuses the profile (409, refused lists them), and so does an entry that closes a listener, trusts an address range or turns a filter off where it would change something, until the body confirms it (409, confirmation true and confirmation_needed lists them). A way back already held - a profile applied and neither reverted nor discarded - refuses it too (409). Then the value each changing entry has is saved as the way back (the hm_settings row ProfileSnapshot), and each entry is written through the setter its REST settings row runs - or, for a [Settings] key no row keeps, through the settings store's locked write - and read back. A setting the server's environment sets is skipped, with the sentence that says so. The answer is {profile, applied, entries: [{store, name, before, after, result, reason, applies}], set, unchanged, skipped, failed, waiting: {reinitialize, restart}, message}: result is set, unchanged, skipped or failed, and waiting how many of the values set wait for a reinitialise or a restart. The apply is one event on the audit trail with the counts, and every setting written is a row of its own; the application log names each. 500 when a write failed part way, with what was written. Administrator password or manage_settings; refused for domain-restricted keys.\","
         "\"requestBody\":{\"required\":false,\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"properties\":{\"confirm\":{\"type\":\"boolean\"}}}}}},"
         "\"responses\":{\"200\":{\"description\":\"What each entry did\"},\"400\":{\"description\":\"The body is not an object, or confirm is not a boolean\"},\"404\":{\"description\":\"No such profile\"},\"409\":{\"description\":\"A way back is held, an entry needs the confirmation, or an entry would be refused\"},\"500\":{\"description\":\"A write failed part way; the way back holds what was changed\"}}}},"
         "\"/api/v1/settings/profiles/revert\":{\"post\":{\"summary\":\"Put back what the last profile changed\","
         "\"description\":\"Writes back the value each setting had before the last profile was applied, through the same setters, and clears the way back. A setting that no longer holds what the profile wrote - an administrator changed it since - is left as it is, and so is one the server's environment now sets; the answer says which (result left, with the reason). A [Settings] key goes back byte for byte, and one that was not stored before is removed, returning it to its default. The answer is {profile, reverted, entries: [{store, name, before, after, result, reason}], set, skipped, failed, message}; the record of the profile says reverted. 409 when there is no way back to put back. On the audit trail as one event with the counts, and a row per setting. Administrator password or manage_settings; refused for domain-restricted keys.\","
         "\"responses\":{\"200\":{\"description\":\"What each setting did\"},\"409\":{\"description\":\"Nothing to revert\"},\"500\":{\"description\":\"The way back could not be read, or a write failed; the way back keeps what is left\"}}}},"
         "\"/api/v1/settings/profiles/discard\":{\"post\":{\"summary\":\"Forget the way back, and keep the profile's settings\","
         "\"description\":\"Clears the way back from the last profile applied, keeping every setting as it is, so that another profile may be applied; the record of the profile says kept. 409 when there is no way back. On the audit trail. Administrator password or manage_settings; refused for domain-restricted keys.\","
         "\"responses\":{\"200\":{\"description\":\"{profile, discarded, entries, message}\"},\"409\":{\"description\":\"Nothing to discard\"}}}}";

      return AnsiString(paths);
   }
}
