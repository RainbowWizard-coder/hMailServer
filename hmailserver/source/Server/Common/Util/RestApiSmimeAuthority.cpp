// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// The REST API of the organisation CA for S/MIME. See OrganisationCa.h and
// docs/SmimeAuthority.md.
//
//    GET    /api/v1/domains/{domain}/smime-ca                   the authority, its roots, what it issued
//    POST   /api/v1/domains/{domain}/smime-ca                   create it          {"key"?: "p256" | "rsa3072"}
//    POST   /api/v1/domains/{domain}/smime-ca/rotate            a new root         {"key"?}
//    DELETE /api/v1/domains/{domain}/smime-ca/issued/{serial}   revoke one         ?reason=
//    POST   /api/v1/me/smime/enroll                             sign the account's request {"csr", "keep"?}
//    POST   /api/v1/me/smime/issued/{serial}/confirm            its key is stored: supersede the others
//    DELETE /api/v1/me/smime/issued/{serial}                    revoke one of its own ?reason=
//    GET    /api/v1/me/directory/{address}/smime                a colleague's certificates
//
// and the authority's part of GET /api/v1/me/smime. The root and the CRL are
// NOT here: they are public, and the web services listener serves them over
// plain HTTP (WebServicesServer::HandleSmimeAuthority_). The domain routes are
// scoped to a credential's domains in Authorize_ and the three that act are
// refused to a read-only key; the account's routes answer an account and
// nothing else, and the three that act answer the account's own sign-in and
// not an app password or an administrator's support session. There is no COM
// surface, so the Control Panel shows none of it; the Control Deck's domain
// editor is where it is managed.

#include "StdAfx.h"

#include "RestApiServer.h"
#include "JsonDocument.h"
#include "OrganisationCa.h"
#include "DirectoryStore.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../Cache/CacheContainer.h"
#include "../Persistence/PersistentDomain.h"

#include <ctime>
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
      AnsiString Number(__int64 value)
      {
         AnsiString text;
         text.Format("%I64d", value);
         return text;
      }

      const char *Bool(bool value)
      {
         return value ? "true" : "false";
      }

      // An absent or empty body is an empty object.
      bool ParseObject(const AnsiString &requestBody, JsonValue &body)
      {
         AnsiString trimmed = requestBody;
         trimmed.Trim();

         std::string text = trimmed.IsEmpty() ? std::string("{}") : std::string(trimmed.c_str(), (size_t) trimmed.GetLength());

         std::string parseError;
         return JsonValue::Parse(text, body, parseError) && body.IsObject();
      }

      // The first member of the body that is not one of the known names, or
      // empty when there is none.
      AnsiString UnknownMember(const JsonValue &body, const char *first, const char *second)
      {
         for (const std::pair<std::string, JsonValue> &member : body.Members())
         {
            if (first != nullptr && member.first == first)
               continue;
            if (second != nullptr && member.first == second)
               continue;
            return AnsiString(member.first.substr(0, 48).c_str());
         }
         return AnsiString();
      }

      const char *StatusOf(const OrganisationCa::Issued &issued, __int64 now)
      {
         if (issued.revoked > 0)
            return "revoked";
         if (issued.notAfter <= now)
            return "expired";
         return "valid";
      }

      AnsiString Serials(const std::vector<__int64> &serials)
      {
         AnsiString json;
         for (size_t i = 0; i < serials.size(); i++)
         {
            if (i > 0)
               json += ",";
            json += Number(serials[i]);
         }
         return json;
      }

      // The sentence for an administrator's support session: whoever holds the
      // key a certificate certifies reads the mail encrypted to it and signs as
      // the person, so a certificate is only ever the person's own doing.
      const char *SupportSessionRefusal =
         "{\"error\":\"not in a support session: a certificate from the domain's authority is requested, confirmed and revoked by the account holder signed in as themselves, since whoever holds its key reads the mail encrypted to it and signs as them\"}";
   }

   // ---- the documents ------------------------------------------------------------

   AnsiString
   RestApiServer::MeSmimeAuthorityJson_(std::shared_ptr<const Account> account)
   {
      if (!account)
         return AnsiString("null");

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(account->GetDomainID());
      const AnsiString domainName = domain ? Utf8_(domain->GetName()) : AnsiString();

      OrganisationCa::Authority authority;
      if (!OrganisationCa::Current(account->GetDomainID(), authority))
         return "{\"domain\":\"" + JsonEscape_(domainName) + "\",\"available\":false}";

      const __int64 now = (__int64) time(nullptr);
      const OrganisationCa::RootFacts facts = OrganisationCa::Facts(authority, false);

      AnsiString json = "{\"domain\":\"" + JsonEscape_(domainName) + "\",\"available\":true" +
                        ",\"subject\":\"" + JsonEscape_(facts.subject) + "\"" +
                        ",\"fingerprint\":\"" + JsonEscape_(facts.fingerprint) + "\"" +
                        ",\"key_type\":\"" + JsonEscape_(facts.keyType) + "\"" +
                        ",\"not_after\":" + Number(facts.notAfter) +
                        ",\"certificate\":\"" + JsonEscape_(authority.certificate) + "\"" +
                        ",\"issued\":[";

      std::vector<OrganisationCa::Issued> issued;
      OrganisationCa::IssuedForAccount(account->GetDomainID(), account->GetID(), issued);
      for (size_t i = 0; i < issued.size(); i++)
      {
         const OrganisationCa::Issued &row = issued[i];
         if (i > 0)
            json += ",";
         json += "{\"serial\":" + Number(row.serial) +
                 ",\"address\":\"" + JsonEscape_(Utf8_(row.address)) + "\"" +
                 ",\"fingerprint\":\"" + JsonEscape_(row.fingerprint) + "\"" +
                 ",\"not_before\":" + Number(row.notBefore) +
                 ",\"not_after\":" + Number(row.notAfter) +
                 ",\"status\":\"" + StatusOf(row, now) + "\"" +
                 ",\"confirmed\":" + AnsiString(Bool(row.pending == OrganisationCa::PendingNone)) +
                 ",\"revoked\":" + Number(row.revoked) +
                 ",\"reason\":\"" + JsonEscape_(Utf8_(row.reason)) + "\"}";
      }

      json += "]}";
      return json;
   }

   // ---- the domain's authority ----------------------------------------------------

   HttpResponse
   RestApiServer::HandleDomainSmimeAuthority_(const Caller &caller, const Route &route, const AnsiString &requestBody)
   {
      std::shared_ptr<Domain> domain = std::shared_ptr<Domain>(new Domain());
      if (route.identifier.IsEmpty() || !PersistentDomain::ReadObject(domain, String(route.identifier)) || domain->GetID() <= 0)
         return BuildResponse_(404, "{\"error\":\"domain not found\"}");

      const __int64 domainId = domain->GetID();
      const String domainName = domain->GetName();

      if (route.kind == RouteSmimeCaCreate || route.kind == RouteSmimeCaRotate)
      {
         JsonValue body;
         if (!ParseObject(requestBody, body))
            return BuildResponse_(400, "{\"error\":\"the body must be a JSON object\"}");

         const AnsiString unknown = UnknownMember(body, "key", nullptr);
         if (!unknown.IsEmpty())
            return BuildResponse_(400, "{\"error\":\"unknown field: " + JsonEscape_(unknown) + "\"}");

         OrganisationCa::KeyKind kind = OrganisationCa::KeyP256;
         const JsonValue *key = body.Get("key");
         if (key && !key->IsNull())
         {
            if (!key->IsString() || (key->AsString() != "p256" && key->AsString() != "rsa3072"))
               return BuildResponse_(400, "{\"error\":\"key: p256 (the default) or rsa3072\"}");
            if (key->AsString() == "rsa3072")
               kind = OrganisationCa::KeyRsa3072;
         }

         OrganisationCa::Authority made;
         String problem;
         const int status = OrganisationCa::Create(domainId, domainName, kind, route.kind == RouteSmimeCaRotate, made, problem);
         if (status != 200 && status != 201)
            return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(Utf8_(problem)) + "\"}");

         LOG_APPLICATION(Formatter::Format(_T("S/MIME authority: {0} {1} a root for {2} ({3})."), String(caller.identity),
            String(route.kind == RouteSmimeCaRotate ? _T("rotated to") : _T("created")), domainName,
            String(kind == OrganisationCa::KeyRsa3072 ? _T("RSA-3072") : _T("P-256"))));

         // Answered as the reading route answers it, below.
         Route reading = route;
         reading.kind = RouteSmimeCaGet;
         HttpResponse document = HandleDomainSmimeAuthority_(caller, reading, AnsiString());
         if (document.status == 200)
            document.status = status;
         return document;
      }

      if (route.kind == RouteSmimeCaRevoke)
      {
         OrganisationCa::Reason reason = OrganisationCa::ReasonKeyCompromise;
         const AnsiString reasonText = QueryParameter_(route.query, "reason");
         if (!reasonText.IsEmpty() && !OrganisationCa::ParseReason(String(reasonText), reason))
            return BuildResponse_(400, "{\"error\":\"reason: keyCompromise (the default), superseded or cessationOfOperation\"}");

         OrganisationCa::Issued revoked;
         String problem;
         const int status = OrganisationCa::Revoke(domainId, route.record_id, 0, reason, revoked, problem);
         if (status != 200)
            return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(Utf8_(problem)) + "\"}");

         return BuildResponse_(200, "{\"revoked\":true,\"serial\":" + Number(revoked.serial) +
            ",\"address\":\"" + JsonEscape_(Utf8_(revoked.address)) + "\",\"reason\":\"" + JsonEscape_(Utf8_(revoked.reason)) + "\"}");
      }

      // The reading: every root the domain has, the certificates they issued,
      // and where the clients fetch the public half.
      std::vector<OrganisationCa::Authority> authorities;
      if (!OrganisationCa::ForDomain(domainId, authorities))
         return BuildResponse_(500, "{\"error\":\"the authority could not be read\"}");

      const __int64 now = (__int64) time(nullptr);
      const String base = OrganisationCa::BaseUrl();
      const AnsiString baseText = Utf8_(base);

      AnsiString current = "null";
      AnsiString retired;
      for (size_t i = 0; i < authorities.size(); i++)
      {
         const OrganisationCa::Authority &authority = authorities[i];
         const OrganisationCa::RootFacts facts = OrganisationCa::Facts(authority, authority.retired == 0);

         AnsiString root = "{\"id\":" + Number(authority.id) +
                           ",\"subject\":\"" + JsonEscape_(facts.subject) + "\"" +
                           ",\"fingerprint\":\"" + JsonEscape_(facts.fingerprint) + "\"" +
                           ",\"key_type\":\"" + JsonEscape_(facts.keyType) + "\"" +
                           ",\"not_before\":" + Number(facts.notBefore) +
                           ",\"not_after\":" + Number(facts.notAfter) +
                           ",\"created\":" + Number(authority.created) +
                           ",\"retired\":" + Number(authority.retired) +
                           ",\"published\":" + Bool(OrganisationCa::IsPublished(authority, now)) +
                           ",\"key_held\":" + Bool(!authority.protectedKey.IsEmpty()) +
                           ",\"root_path\":\"" + JsonEscape_(OrganisationCa::RootPath(authority.fingerprint, "pem")) + "\"" +
                           ",\"crl_path\":\"" + JsonEscape_(OrganisationCa::CrlPath(authority.fingerprint)) + "\"";

         if (authority.retired == 0)
         {
            root += ",\"key_opens\":" + AnsiString(Bool(facts.keyOpens)) +
                    ",\"next_serial\":" + Number(authority.serial) +
                    ",\"certificate\":\"" + JsonEscape_(authority.certificate) + "\"" +
                    ",\"pem\":\"" + JsonEscape_(OrganisationCa::Pem(authority.certificate, "CERTIFICATE")) + "\"}";
            current = root;
         }
         else
         {
            root += "}";
            if (!retired.IsEmpty())
               retired += ",";
            retired += root;
         }
      }

      std::vector<OrganisationCa::Issued> issued;
      OrganisationCa::IssuedForDomain(domainId, 200, issued);

      AnsiString issuedJson;
      for (size_t i = 0; i < issued.size(); i++)
      {
         const OrganisationCa::Issued &row = issued[i];
         if (i > 0)
            issuedJson += ",";
         issuedJson += "{\"serial\":" + Number(row.serial) +
                       ",\"address\":\"" + JsonEscape_(Utf8_(row.address)) + "\"" +
                       ",\"account_id\":" + Number(row.accountId) +
                       ",\"authority_id\":" + Number(row.caId) +
                       ",\"fingerprint\":\"" + JsonEscape_(row.fingerprint) + "\"" +
                       ",\"not_before\":" + Number(row.notBefore) +
                       ",\"not_after\":" + Number(row.notAfter) +
                       ",\"status\":\"" + StatusOf(row, now) + "\"" +
                       ",\"confirmed\":" + AnsiString(Bool(row.pending == OrganisationCa::PendingNone)) +
                       ",\"revoked\":" + Number(row.revoked) +
                       ",\"reason\":\"" + JsonEscape_(Utf8_(row.reason)) + "\"}";
      }

      AnsiString json = "{\"domain\":\"" + JsonEscape_(Utf8_(domainName)) + "\"" +
                        ",\"authority\":" + current +
                        ",\"retired\":[" + retired + "]" +
                        ",\"issued\":[" + issuedJson + "]" +
                        ",\"base_url\":\"" + JsonEscape_(baseText) + "\"" +
                        ",\"root_path\":\"" + JsonEscape_(OrganisationCa::CurrentPath(domainName, "pem")) + "\"" +
                        ",\"crl_path\":\"" + JsonEscape_(OrganisationCa::CurrentPath(domainName, "crl")) + "\"" +
                        ",\"root_url\":\"" + (base.IsEmpty() ? AnsiString() : JsonEscape_(baseText + OrganisationCa::CurrentPath(domainName, "pem"))) + "\"" +
                        ",\"crl_url\":\"" + (base.IsEmpty() ? AnsiString() : JsonEscape_(baseText + OrganisationCa::CurrentPath(domainName, "crl"))) + "\"}";

      return BuildResponse_(200, json);
   }

   // ---- the account's own ------------------------------------------------------------

   HttpResponse
   RestApiServer::HandleMeSmimeEnroll_(const Caller &caller, const AnsiString &requestBody)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      HttpResponse refusal;
      if (RefuseAppPasswordCaller_(caller, "An S/MIME certificate request", refusal))
         return refusal;
      if (!caller.support_by.IsEmpty())
         return BuildResponse_(403, SupportSessionRefusal);

      JsonValue body;
      std::string parseError;
      if (!JsonValue::Parse(std::string(requestBody.c_str(), (size_t) requestBody.GetLength()), body, parseError) || !body.IsObject())
         return BuildResponse_(400, "{\"error\":\"a JSON object is expected\"}");

      const AnsiString unknown = UnknownMember(body, "csr", "keep");
      if (!unknown.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"unknown field: " + JsonEscape_(unknown) + "\"}");

      const JsonValue *csr = body.Get("csr");
      if (!csr || !csr->IsString() || csr->AsString().empty())
         return BuildResponse_(400, "{\"error\":\"csr: the PKCS#10 certificate request the page made, base64 DER or PEM\"}");

      const JsonValue *keepValue = body.Get("keep");
      if (keepValue && !keepValue->IsNull() && !keepValue->IsBool())
         return BuildResponse_(400, "{\"error\":\"keep: true to keep the certificates this address already has\"}");
      const bool keep = keepValue && keepValue->AsBool();

      OrganisationCa::Issued made;
      std::vector<__int64> supersedes;
      String problem;
      const int status = OrganisationCa::Enrol(account, AnsiString(csr->AsString().c_str(), (int) csr->AsString().size()), keep, made, supersedes, problem);
      if (status != 201)
         return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(Utf8_(problem)) + "\"}");

      OrganisationCa::Authority authority;
      OrganisationCa::Find(account->GetDomainID(), made.caId, authority);

      AnsiString json = "{\"certificate\":\"" + JsonEscape_(made.certificate) + "\"" +
                        ",\"chain\":[" + (authority.certificate.IsEmpty() ? AnsiString() : "\"" + JsonEscape_(authority.certificate) + "\"") + "]" +
                        ",\"serial\":" + Number(made.serial) +
                        ",\"fingerprint\":\"" + JsonEscape_(made.fingerprint) + "\"" +
                        ",\"address\":\"" + JsonEscape_(Utf8_(made.address)) + "\"" +
                        ",\"not_before\":" + Number(made.notBefore) +
                        ",\"not_after\":" + Number(made.notAfter) +
                        ",\"confirmed\":false" +
                        ",\"supersedes\":[" + Serials(supersedes) + "]}";

      return BuildResponse_(201, json);
   }

   HttpResponse
   RestApiServer::HandleMeSmimeIssuedConfirm_(const Caller &caller, __int64 serial)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      HttpResponse refusal;
      if (RefuseAppPasswordCaller_(caller, "An S/MIME certificate confirmation", refusal))
         return refusal;
      if (!caller.support_by.IsEmpty())
         return BuildResponse_(403, SupportSessionRefusal);

      std::vector<__int64> superseded;
      String problem;
      const int status = OrganisationCa::Confirm(account, serial, superseded, problem);
      if (status != 200)
         return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(Utf8_(problem)) + "\"}");

      return BuildResponse_(200, "{\"confirmed\":true,\"serial\":" + Number(serial) + ",\"superseded\":[" + Serials(superseded) + "]}");
   }

   HttpResponse
   RestApiServer::HandleMeSmimeIssuedRevoke_(const Caller &caller, __int64 serial, const AnsiString &query)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      HttpResponse refusal;
      if (RefuseAppPasswordCaller_(caller, "An S/MIME certificate revocation", refusal))
         return refusal;
      if (!caller.support_by.IsEmpty())
         return BuildResponse_(403, SupportSessionRefusal);

      OrganisationCa::Reason reason = OrganisationCa::ReasonKeyCompromise;
      const AnsiString reasonText = QueryParameter_(query, "reason");
      if (!reasonText.IsEmpty() && !OrganisationCa::ParseReason(String(reasonText), reason))
         return BuildResponse_(400, "{\"error\":\"reason: keyCompromise (the default), superseded or cessationOfOperation\"}");

      OrganisationCa::Issued revoked;
      String problem;
      const int status = OrganisationCa::Revoke(account->GetDomainID(), serial, account->GetID(), reason, revoked, problem);
      if (status != 200)
         return BuildResponse_(status, "{\"error\":\"" + JsonEscape_(Utf8_(problem)) + "\"}");

      return BuildResponse_(200, "{\"revoked\":true,\"serial\":" + Number(revoked.serial) +
         ",\"reason\":\"" + JsonEscape_(Utf8_(revoked.reason)) + "\"}");
   }

   HttpResponse
   RestApiServer::HandleMeDirectorySmime_(const Caller &caller, const String &addressText)
   {
      std::shared_ptr<const Account> account = caller.account;
      if (!account)
         return BuildResponse_(500, "{\"error\":\"internal error\"}");

      // The directory's switch decides this route as it decides the photo's.
      if (!DirectoryStore::Enabled())
         return BuildResponse_(404, "{\"error\":\"no such directory entry\"}");

      String address = addressText;
      address.Trim();
      address.MakeLower();

      // Of the reader's own domain, an account that can sign in and still owns
      // the address, and a certificate that is confirmed and neither revoked nor
      // expired - one query and a check per row. The opt-out is not asked: it
      // takes a person out of the listing, not out of reach of somebody who
      // already has the address. Every other answer is the same 404.
      std::vector<OrganisationCa::Issued> issued;
      if (!OrganisationCa::Published(account->GetDomainID(), address, issued) || issued.empty())
         return BuildResponse_(404, "{\"error\":\"no certificate from this domain's authority for that address\"}");

      AnsiString certificates;
      AnsiString chain;
      for (size_t i = 0; i < issued.size(); i++)
      {
         const OrganisationCa::Issued &row = issued[i];
         if (i > 0)
            certificates += ",";
         certificates += "{\"serial\":" + Number(row.serial) +
                         ",\"fingerprint\":\"" + JsonEscape_(row.fingerprint) + "\"" +
                         ",\"not_before\":" + Number(row.notBefore) +
                         ",\"not_after\":" + Number(row.notAfter) +
                         ",\"certificate\":\"" + JsonEscape_(row.certificate) + "\"" +
                         ",\"pem\":\"" + JsonEscape_(OrganisationCa::Pem(row.certificate, "CERTIFICATE")) + "\"}";

         if (chain.IsEmpty())
         {
            OrganisationCa::Authority authority;
            if (OrganisationCa::Find(account->GetDomainID(), row.caId, authority))
               chain = "\"" + JsonEscape_(authority.certificate) + "\"";
         }
      }

      return BuildResponse_(200, "{\"address\":\"" + JsonEscape_(Utf8_(address)) + "\",\"certificates\":[" + certificates + "],\"chain\":[" + chain + "]}");
   }

   // ---- the description ----------------------------------------------------------------

   AnsiString
   RestApiServer::OpenApiSmimeAuthorityPaths_()
   {
      static const char *paths =
         ",\"/api/v1/domains/{domain}/smime-ca\":{"
         "\"get\":{\"summary\":\"A domain's S/MIME authority\",\"description\":\"The organisation CA for S/MIME (docs/SmimeAuthority.md): authority, the current root (null when the domain has none) with id, subject, fingerprint (SHA-256 of the DER, which is also its name in the public URLs), key_type, not_before, not_after, created, published, key_held, key_opens (whether its private key opens on this machine; false after a database or a backup was carried from a machine where the key's envelope does not open, and the authority must then be rotated), next_serial, certificate (base64 DER), pem, root_path and crl_path (/smime-ca/{fingerprint}.pem and .crl under the web services listener); retired, the roots a rotation replaced, each with published (true while a certificate it signed has not expired - only then does it sign a CRL, stay trusted by the webmail's chain check and keep its key, which is wiped afterwards: key_held) and its paths; issued, the newest 200 certificates with serial, address (empty once erased), account_id (0 once the account was deleted), authority_id, fingerprint, not_before, not_after, status (valid, revoked or expired), confirmed (false while the key has not been stored, when it is neither answered to colleagues nor superseding anything), revoked and reason; base_url, smime_authority_base_url as the certificates are written with it (empty: no CRL distribution point and no caIssuers URL in them); root_path and crl_path, the current root's and its CRL's paths by the domain's name (/smime-ca/{domain}.pem and .crl; .cer is the root in DER), and root_url and crl_url, the same under base_url (empty while it is). Times are seconds since the epoch. The root and the CRL themselves are public and are served over plain HTTP by the web services listener, never by this listener.\",\"responses\":{\"200\":{\"description\":\"domain, authority, retired, issued, base_url, root_path, crl_path, root_url, crl_url\"},\"404\":{\"description\":\"No such domain\"}}},"
         "\"post\":{\"summary\":\"Create a domain's S/MIME authority\",\"description\":\"Body: key, p256 (the default) or rsa3072. A self-signed root, CN '{domain} S/MIME Authority', CA:TRUE pathlen:0, keyCertSign and cRLSign, extendedKeyUsage emailProtection and a critical name constraint permitting the domain's own addresses (none for a domain whose name is not plain ASCII), ten years. Its private key is stored protected by the platform's secret store directly - DPAPI with the machine's key on Windows, which opens on this machine only, or the stored-secret key file on Linux, which opens wherever that file is - and never under the legacy scheme: when the store is unavailable the answer is 503 and nothing is created. From then on the domain's accounts may enrol from the webmail. Any other field is refused.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"key\":{\"type\":\"string\",\"enum\":[\"p256\",\"rsa3072\"]}}}}}},\"responses\":{\"201\":{\"description\":\"Created: the document the GET answers\"},\"400\":{\"description\":\"A field refused\"},\"404\":{\"description\":\"No such domain\"},\"409\":{\"description\":\"The domain has an authority already; rotate it\"},\"503\":{\"description\":\"The machine's secret store could not protect the key; nothing was created\"}}}},"
         "\"/api/v1/domains/{domain}/smime-ca/rotate\":{"
         "\"post\":{\"summary\":\"Rotate a domain's S/MIME authority\",\"description\":\"Body: key, as for the create. A new root replaces the current one for every enrolment from now on and takes over its serial counter; the old root is retired, not deleted - while a certificate it signed has not expired it keeps its key, signs its own CRL, stays published and is trusted by the webmail's chain check for those certificates (a revoked one never), and after that its key is wiped and it is trusted no more. A root rotated because its key may have been exposed must also be taken off the clients, and the certificates it signed revoked. Clients that were given the old root must be given the new one (docs/SmimeAuthority.md, Rotation).\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{\"key\":{\"type\":\"string\",\"enum\":[\"p256\",\"rsa3072\"]}}}}}},\"responses\":{\"200\":{\"description\":\"Rotated: the document the GET answers\"},\"400\":{\"description\":\"A field refused\"},\"404\":{\"description\":\"No such domain, or it has no authority to rotate\"},\"503\":{\"description\":\"The machine's secret store could not protect the key; nothing changed\"}}}},"
         "\"/api/v1/domains/{domain}/smime-ca/issued/{serial}\":{"
         "\"delete\":{\"summary\":\"Revoke a certificate the domain's authority issued\",\"description\":\"By its serial, which names one certificate of the domain across its roots. The query parameter reason is keyCompromise (the default), superseded or cessationOfOperation; it is written into the root's CRL, which is made again at the next fetch, and from then on the webmail's chain check refuses the certificate and a colleague's page stops encrypting to it. A certificate revoked already keeps the date and reason of its first revocation. Recorded on the audit trail.\",\"parameters\":[{\"name\":\"reason\",\"in\":\"query\",\"schema\":{\"type\":\"string\",\"enum\":[\"keyCompromise\",\"superseded\",\"cessationOfOperation\"]}}],\"responses\":{\"200\":{\"description\":\"revoked, serial, address, reason\"},\"400\":{\"description\":\"An unknown reason\"},\"404\":{\"description\":\"No such domain or certificate\"},\"409\":{\"description\":\"Revoked already\"}}}},"
         "\"/api/v1/me/smime/enroll\":{"
         "\"post\":{\"summary\":\"Have the domain's S/MIME authority sign the account's own key\",\"description\":\"Body: csr, the PKCS#10 request the webmail made for a key pair it generated in the browser (base64 DER or PEM) - the private key never comes to the server, which signs and never generates a person's key - and keep, true to keep the certificates the address already has (a second device). The request must verify with its own key, carry an RSA key of 2048 to 8192 bits or a P-256 or P-384 key, and name exactly one ASCII address of at most 128 characters - the account's own or an active alias of its own domain that points at it; not an address another account granted, a proved external address or a masked one. Issued for one year: the subject is the account's name as this server holds it and the address, with rfc822Name, emailProtection, and - when smime_authority_base_url is set - a CRL distribution point and an authority-information-access caIssuers URL naming the root by its fingerprint. The certificate is PENDING: it is not answered to colleagues and supersedes nothing until it is confirmed, which the account's own store does when the key is put in it (PUT /api/v1/me/smime/own with the certificate) and POST /api/v1/me/smime/issued/{serial}/confirm does for a client that keeps its key elsewhere; then, unless keep, the address's earlier certificates are revoked as superseded. At most ten valid certificates per account and five enrolments a day. The account's own sign-in only: not an app password, not an administrator's support session.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"csr\"],\"additionalProperties\":false,\"properties\":{\"csr\":{\"type\":\"string\"},\"keep\":{\"type\":\"boolean\"}}}}}},\"responses\":{\"201\":{\"description\":\"certificate (base64 DER), chain (the root, base64 DER), serial, fingerprint, address, not_before, not_after, confirmed (false), supersedes (the serials its confirmation will revoke)\"},\"400\":{\"description\":\"The request refused; error says why\"},\"403\":{\"description\":\"An app password or a support session\"},\"404\":{\"description\":\"The domain has no S/MIME authority\"},\"409\":{\"description\":\"The account holds as many valid certificates as it may\"},\"429\":{\"description\":\"The account has enrolled five times in the last day\"},\"503\":{\"description\":\"The authority's key does not open on this machine; an administrator rotates it\"}}}},"
         "\"/api/v1/me/smime/issued/{serial}/confirm\":{"
         "\"post\":{\"summary\":\"Confirm that the key of a certificate the authority issued to the account is stored\",\"description\":\"For a client that keeps its key outside the account's store (the webmail's store confirms by itself when the key is put in it). From then on the certificate is answered to colleagues, and unless the enrolment said keep, the address's other valid certificates are revoked as superseded. Confirming a confirmed certificate changes nothing. The account's own sign-in only.\",\"responses\":{\"200\":{\"description\":\"confirmed, serial, superseded (the serials it revoked)\"},\"403\":{\"description\":\"An app password or a support session\"},\"404\":{\"description\":\"No such certificate of this account\"},\"409\":{\"description\":\"The certificate is revoked\"}}}},"
         "\"/api/v1/me/smime/issued/{serial}\":{"
         "\"delete\":{\"summary\":\"Revoke a certificate the authority issued to the account\",\"description\":\"The account's own only; another account's is the same 404 as none. reason as the administrator's route takes it, keyCompromise by default. The account's own sign-in only.\",\"parameters\":[{\"name\":\"reason\",\"in\":\"query\",\"schema\":{\"type\":\"string\",\"enum\":[\"keyCompromise\",\"superseded\",\"cessationOfOperation\"]}}],\"responses\":{\"200\":{\"description\":\"revoked, serial, reason\"},\"400\":{\"description\":\"An unknown reason\"},\"403\":{\"description\":\"An app password or a support session\"},\"404\":{\"description\":\"No such certificate of this account\"},\"409\":{\"description\":\"Revoked already\"}}}},"
         "\"/api/v1/me/directory/{address}/smime\":{"
         "\"get\":{\"summary\":\"A colleague's certificates from the domain's S/MIME authority\",\"description\":\"By address, for encrypting to them: the confirmed, valid certificates the reader's own domain's authority issued for that address, when it belongs to an active account of the reader's own domain and is still that account's (its own address, or an active alias pointing at it) - asked whether or not that person opted out of the directory's listing, since the opt-out hides a person from the list and not from somebody who already has the address. certificates, each with serial, fingerprint, not_before, not_after, certificate (base64 DER) and pem, and chain (the root that signed them). 404 while directory_enabled is off, for an address of another domain, an inactive account, an address the account no longer holds, and a certificate pending, revoked or expired - the same answer each time.\",\"responses\":{\"200\":{\"description\":\"address, certificates, chain\"},\"404\":{\"description\":\"No such certificate to answer\"}}}}";

      return AnsiString(paths);
   }
}
