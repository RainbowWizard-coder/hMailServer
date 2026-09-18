// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The REST API's sample domain. See RestApiServer.h and
// Common/Application/SampleDomain.h.
//
//    GET    /api/v1/setup/sample-domain    is it there, and what is in it
//    POST   /api/v1/setup/sample-domain    make it
//    DELETE /api/v1/setup/sample-domain    remove it, and say what went
//
// The route is the server's rather than a domain's: the sample makes a domain
// this server does not have yet, so a credential confined to the domains it does
// have names nothing here and is refused. Reading needs view_configuration, as
// reading the checklist beside it does; the two that change anything need
// manage_server, which is the server administrator and nobody else, because they
// create three mailboxes with a password and delete a whole domain with
// everything in it.
//
// The GET document deliberately carries no password. The one the mailboxes get
// is minted per creation and is in the 201 answer alone; view_configuration is a
// weaker permission than manage_server, and a route that read back the
// credentials of three relay-capable mailboxes to everyone who may look at the
// configuration would be handing them out.
//
// Neither takes a body, and neither takes a parameter. There is exactly one
// sample domain, it is always example.test, and what is in it is fixed - a sample
// with options is a configuration screen, which is the thing this row exists to
// let somebody skip.
//
// The three answers, for each of the two that change something:
//    200/201  it was done; the body is the report
//    409      it was refused, and "error" says why - already there, not there, or
//             an example.test this did not make, which is never removed
//    500      the server could not do it; the error log has the rest
#include "StdAfx.h"
#include "RestApiServer.h"
#include "../Application/SampleDomain.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   HttpResponse
   RestApiServer::HandleSampleDomain_(const Route &route)
   {
      if (route.kind == RouteSampleDomainGet)
         return BuildResponse_(200, SampleDomain::StateJson());

      AnsiString json;
      String sentence;

      const bool creating = route.kind == RouteSampleDomainCreate;
      const SampleDomain::Outcome outcome = creating
         ? SampleDomain::Create(json, sentence)
         : SampleDomain::Remove(json, sentence);

      if (outcome == SampleDomain::OutcomeDone)
         return BuildResponse_(creating ? 201 : 200, json);

      // A refusal and a failure carry the same shape: the sentence, and the
      // state the server is in now, so that a page can redraw its buttons from
      // the answer it already has rather than asking again.
      AnsiString body = "{\"error\":\"";
      body += JsonEscape_(Utf8_(sentence));
      body += "\",\"sample\":";
      body += json.IsEmpty() ? SampleDomain::StateJson() : json;
      body += "}";

      return BuildResponse_(outcome == SampleDomain::OutcomeRefused ? 409 : 500, body);
   }

   AnsiString
   RestApiServer::OpenApiSampleDomainPaths_()
   {
      static const char *paths =
         ",\"/api/v1/setup/sample-domain\":{"
         "\"get\":{\"summary\":\"Whether the sample domain is there, and what is in it\","
         "\"description\":\"state is present when example.test is here and this server made it, foreign when a domain of that name is here and it did not, absent when there is none, and unknown when the domain table could not be read - which is deliberately not the same answer as absent. exists says whether a domain of that name is on the server at all, whoever made it; domain_id is the id of the sample's own domain and 0 otherwise. accounts and list are the fixed contents and are the same on every server. There is no password here: the one the mailboxes get is minted when the sample is made and shown once, in the answer to POST, and nothing keeps the clear text - password_shown_once says so. Server-wide; refused for domain-restricted keys.\","
         "\"responses\":{\"200\":{\"description\":\"{state, domain, domain_id, exists, accounts, list, password_shown_once}\"}}},"
         "\"post\":{\"summary\":\"Make the sample domain\","
         "\"description\":\"Creates example.test with three accounts (alice, bob and carol, all with one password), a public distribution list of the three, one rule on the first account that files a subject carrying the word Invoice into a folder of that name, and eight messages queued through the server's own delivery path - so the list expands, the rule fires and the folder it names is created by the rule itself. The messages are in the mailboxes a few seconds after this answers. The password is minted here, twenty random characters, and is in password and in message: this is the only time any route says it, because it is hashed into the accounts and the clear text is not stored. What it made is recorded in the setting sample_domain_id, which is what lets DELETE remove exactly that and nothing else; the first-hour checklist leaves the sample out of its counts. 409 when example.test already exists, whoever made it: a domain this did not make is never taken over. Nothing outside the domain is touched - no listener, no certificate, no route, no other setting. The sample is still three ordinary mailboxes on a server whose default address range lets an authenticated session relay to the Internet: remove it before this server takes real mail. Administrator password or manage_server; no body.\","
         "\"responses\":{\"201\":{\"description\":\"{created, domain, domain_id, password, accounts, accounts_created, list, list_members, rule, rule_account, rule_folder, rules, messages, deliveries, message}\"},"
         "\"409\":{\"description\":\"example.test already exists\"},\"500\":{\"description\":\"The server could not make it\"}}},"
         "\"delete\":{\"summary\":\"Remove the sample domain, and say what went\","
         "\"description\":\"Removes the domain recorded in sample_domain_id when it is still named example.test, with its accounts, their mail and folders, its aliases, its distribution lists and its rules, and answers how many of each went - so a person is told what was taken away rather than being asked to trust it. The sample's own mail still in the delivery queue goes too, counted as queued: a message whose every recipient is one of these mailboxes is dropped rather than left addressed to an account that has gone, and one with an outside recipient on it is left alone. An example.test this server did not make is refused with 409 and left alone; so is a server with no sample. A marker whose domain has already been deleted by hand is cleared and reported as such - but a domain table that could not be READ is answered with 500 and nothing is changed, because clearing the marker there would orphan a sample that is still on the server. Anything a learner added to the domain in the meantime goes with it, and is counted in the answer. Administrator password or manage_server.\","
         "\"responses\":{\"200\":{\"description\":\"{removed, domain, domain_id, accounts, lists, rules, messages, queued, message}\"},"
         "\"409\":{\"description\":\"There is nothing to remove, or example.test is not the sample's\"},\"500\":{\"description\":\"The server could not remove it, or could not read the domain table\"}}}}";

      return AnsiString(paths);
   }
}
