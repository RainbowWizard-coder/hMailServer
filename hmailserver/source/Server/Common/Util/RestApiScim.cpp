// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// SCIM 2.0 provisioning (RFC 7643 and RFC 7644): the organisation's identity
// provider - Entra ID, Okta, Google - creating, renaming, suspending and
// removing accounts, with their aliases, and distribution lists, as people
// join, move and leave. See RestApiServer.h and docs/Scim.md.
//
//    GET    /scim/v2/ServiceProviderConfig   what this server supports
//    GET    /scim/v2/Schemas[/{id}]          the User and Group schemas, as far as kept
//    GET    /scim/v2/ResourceTypes[/{id}]    User and Group
//    GET    /scim/v2/Users                   filter, startIndex, count
//    POST   /scim/v2/Users                   an account
//    GET    /scim/v2/Users/{id}              one; the id is the address
//    PUT    /scim/v2/Users/{id}              replaced; a new userName renames it
//    PATCH  /scim/v2/Users/{id}              add, replace and remove
//    DELETE /scim/v2/Users/{id}              the account, its aliases, its list rows
//    ...and the same five for /scim/v2/Groups: the distribution lists.
//
// WHO. An API key and nothing else, whose role holds provision: Authorize_
// answers every other credential 401 before a permission is asked. No SCIM path
// names a domain, so a key confined to named domains is confined here - every
// address it writes must be at a domain it reaches, every listing is filtered to
// them, and a resource elsewhere is the same 404 as one that is not there, and
// an address elsewhere the same 400 as a domain this server does not host. And
// no key changes or removes an account whose authority it does not take in
// (ScimMayChange, RoleManager::Encompasses).
//
// ALL OR NOTHING. A change to an account is judged whole before any of it is
// written, and what a change that fails part way wrote is put back, but for a
// rename, which is reported (ScimCommitUser).
//
// THE MAPPING. A User is an account: id and userName the address; name the
// first and last names; displayName the two composed; active the account's
// switch, whose clearing is a suspension that ends the account's sessions;
// emails the address (work, primary) and the aliases whose value is the address
// (other); password write-only; externalId hm_accounts.accountexternalid. A
// Group is a distribution list: id and displayName the list's address, members
// its confirmed members by address. What neither schema keeps is accepted and
// not kept, and the attribute and the path that name nothing are refused.
//
// THE PERSISTENCE is every other interface's: PersistentAccount, PersistentAlias,
// PersistentDistributionList and PersistentDistributionListRecipient, and a
// rename is MailboxMove::Move. So the audit trail records every write with
// nothing written for it here: ProcessRequest_ installs the key as the actor,
// and the statements land on audited tables.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "DavSupport.h"
#include "JsonDocument.h"
#include "Unicode.h"
#include "Crypt.h"
#include "Time.h"
#include "PasswordPolicy.h"
#include "PasswordHistory.h"
#include "PasswordBreachCheck.h"
#include "MailboxKeys.h"
#include "SignInStore.h"
#include "DiscoveryHolds.h"
#include "../AntiSpam/SpamTrapMonitor.h"
#include "../Application/RoleManager.h"
#include "../BO/Account.h"
#include "../BO/Alias.h"
#include "../BO/Aliases.h"
#include "../BO/Domain.h"
#include "../BO/Domains.h"
#include "../BO/DistributionList.h"
#include "../BO/DistributionLists.h"
#include "../BO/DistributionListRecipient.h"
#include "../BO/DistributionListRecipients.h"
#include "../Persistence/PersistentAccount.h"
#include "../Persistence/PersistentAlias.h"
#include "../Persistence/PersistentDistributionList.h"
#include "../Persistence/PersistentDistributionListRecipient.h"
#include "../Persistence/PersistenceMode.h"
#include "../Persistence/PreSaveLimitationsCheck.h"
#include "../Persistence/MailboxMove.h"

#include <openssl/rand.h>

#include <cctype>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace
{
   using namespace HM;

   const char *const ScimUserSchema = "urn:ietf:params:scim:schemas:core:2.0:User";
   const char *const ScimGroupSchema = "urn:ietf:params:scim:schemas:core:2.0:Group";
   const char *const ScimListResponseSchema = "urn:ietf:params:scim:api:messages:2.0:ListResponse";
   const char *const ScimErrorSchema = "urn:ietf:params:scim:api:messages:2.0:Error";
   const char *const ScimServiceProviderConfigSchema = "urn:ietf:params:scim:schemas:core:2.0:ServiceProviderConfig";
   const char *const ScimResourceTypeSchema = "urn:ietf:params:scim:schemas:core:2.0:ResourceType";
   const char *const ScimSchemaSchema = "urn:ietf:params:scim:schemas:core:2.0:Schema";

   // A path may carry the core schema's URN before its attribute (RFC 7644
   // section 3.10); written lower-case, since a path is compared that way.
   const char *const ScimUserPathPrefix = "urn:ietf:params:scim:schemas:core:2.0:user:";
   const char *const ScimGroupPathPrefix = "urn:ietf:params:scim:schemas:core:2.0:group:";

   // ServiceProviderConfig's filter.maxResults, and a listing's page when the
   // request names no count.
   const int ScimMaxResults = 200;

   // The widths of accountpersonfirstname, accountpersonlastname and
   // accountexternalid in every create script.
   const int ScimMaxPersonName = 60;
   const int ScimMaxExternalId = 255;

   // The attributes of RFC 7643's User that this server does not keep. A body or
   // an operation naming one is accepted and the value is not kept - Entra and
   // Okta send several of them by default - and a resource answered never
   // carries one. docs/Scim.md names them, and says which mappings to remove.
   const char *const ScimUserNotKept[] =
   {
      "nickName", "profileUrl", "title", "userType", "preferredLanguage", "locale", "timezone",
      "phoneNumbers", "ims", "photos", "addresses", "groups", "entitlements", "roles", "x509Certificates"
   };

   // Everything one request is decided with, copied from the credential by the
   // member function that may read it.
   struct ScimContext
   {
      ScimContext() : allowed(nullptr) { }

      std::vector<String> domains;                                   // the key's; empty is every domain
      bool (*allowed)(const std::vector<String> &, const String &);  // RestApiServer::IsDomainAllowed_
      RoleGrant grant;                                               // the key's authority
      AnsiString base;                                               // the URL /scim/v2 is at
      String identity;                                               // key:<id>, for the log

      bool Reaches(const String &domainName) const { return allowed(domains, domainName); }
   };

   // A refusal, in RFC 7644's terms: the status, the scimType where one fits,
   // and the sentence.
   struct ScimRefusal
   {
      ScimRefusal() : status(0) { }

      int status;
      AnsiString scim_type;
      String detail;
   };

   bool ScimRefuse(ScimRefusal &refusal, int status, const char *scimType, const String &detail)
   {
      refusal.status = status;
      refusal.scim_type = scimType;
      refusal.detail = detail;
      return false;
   }

   // ------------------------------------------------------------------ text

   String ScimWide(const std::string &utf8)
   {
      String value;
      Unicode::MultiByteToWide(AnsiString(utf8.c_str()), value);
      return value;
   }

   AnsiString ScimUtf8(const String &value)
   {
      AnsiString utf8;
      Unicode::WideToMultiByte(value, utf8);
      return utf8;
   }

   std::string ScimLower(const std::string &text)
   {
      std::string lowered = text;
      for (size_t i = 0; i < lowered.size(); i++)
         lowered[i] = (char) tolower((unsigned char) lowered[i]);
      return lowered;
   }

   std::string ScimTrim(const std::string &text)
   {
      const size_t first = text.find_first_not_of(" \t\r\n");
      if (first == std::string::npos)
         return std::string();

      const size_t last = text.find_last_not_of(" \t\r\n");
      return text.substr(first, last - first + 1);
   }

   // Compared without regard to case: RFC 7643 section 2.1 makes attribute
   // names case-insensitive, and every client spells them its own way.
   bool ScimSameName(const std::string &name, const char *wanted)
   {
      return ScimLower(name) == ScimLower(std::string(wanted));
   }

   bool ScimStartsWith(const std::string &text, const char *prefix)
   {
      const size_t length = strlen(prefix);
      return text.size() >= length && ScimLower(text.substr(0, length)) == ScimLower(std::string(prefix));
   }

   bool ScimHasLineBreak(const String &text)
   {
      return text.Find(_T("\r")) >= 0 || text.Find(_T("\n")) >= 0;
   }

   bool ScimContainsNoCase(const std::vector<String> &values, const String &value)
   {
      for (const String &existing : values)
      {
         if (existing.CompareNoCase(value) == 0)
            return true;
      }

      return false;
   }

   void ScimRemoveNoCase(std::vector<String> &values, const String &value)
   {
      std::vector<String> kept;
      for (const String &existing : values)
      {
         if (existing.CompareNoCase(value) != 0)
            kept.push_back(existing);
      }

      values.swap(kept);
   }

   // A JSON string of this server's text; the escaper is the listener's own.
   AnsiString ScimQuote(const String &value)
   {
      return "\"" + RestApiServer::JsonEscapeUtf8(value) + "\"";
   }

   // ASCII this file made - a URL whose non-ASCII was percent-encoded, a
   // version - as the inside of a JSON string.
   AnsiString ScimAsciiEscape(const AnsiString &value)
   {
      AnsiString escaped;
      for (int i = 0; i < value.GetLength(); i++)
      {
         const char c = value.GetAt(i);
         if (c == '"' || c == '\\')
            escaped += '\\';
         escaped += c;
      }

      return escaped;
   }

   String ScimOutsideSentence(const wchar_t *attribute)
   {
      // One sentence for a domain this server does not host and for one the key
      // does not reach, so that a confined key learns nothing about another
      // tenant's domains from the refusal.
      return String(attribute) + _T(" must be an address at a domain this api key provisions");
   }

   // ------------------------------------------------------------------ JSON in

   // A member by name, compared without regard to case.
   const JsonValue *ScimAttr(const JsonValue &object, const char *name)
   {
      if (!object.IsObject())
         return nullptr;

      for (const std::pair<std::string, JsonValue> &member : object.Members())
      {
         if (ScimSameName(member.first, name))
            return &member.second;
      }

      return nullptr;
   }

   bool ScimReadText(const JsonValue &value, String &text)
   {
      if (!value.IsString())
         return false;

      const std::string &utf8 = value.AsString();
      if (utf8.find('\0') != std::string::npos)
         return false;

      text = ScimWide(utf8);
      return true;
   }

   // A boolean as SCIM clients send one: true or false, and the strings "True"
   // and "False" that Entra sent in their place before its 2020 compliance fix.
   bool ScimReadFlag(const JsonValue &value, bool &flag)
   {
      if (value.IsBool())
      {
         flag = value.AsBool();
         return true;
      }

      if (value.IsString())
      {
         const std::string lowered = ScimLower(ScimTrim(value.AsString()));
         if (lowered == "true" || lowered == "false")
         {
            flag = lowered == "true";
            return true;
         }
      }

      return false;
   }

   bool ScimParseBody(const AnsiString &requestBody, JsonValue &body)
   {
      std::string parseError;
      std::string text(requestBody.c_str(), (size_t) requestBody.GetLength());
      return JsonValue::Parse(text, body, parseError) && body.IsObject();
   }

   bool ScimIgnoredUserAttribute(const std::string &name)
   {
      // What a client may send back as it read it, and which a write ignores.
      if (ScimSameName(name, "schemas") || ScimSameName(name, "id") || ScimSameName(name, "meta"))
         return true;

      // An extension's attributes - the enterprise user's manager and
      // department among them - which this server does not keep.
      if (ScimStartsWith(name, "urn:"))
         return true;

      for (const char *notKept : ScimUserNotKept)
      {
         if (ScimSameName(name, notKept))
            return true;
      }

      return false;
   }

   // ------------------------------------------------------------------ filters

   // One comparison: attribute op value, where value is a JSON string or true or
   // false. The attribute and the operator lower-case. A filter this reads is not
   // yet one a collection answers: see ScimUserFilterAnswered.
   struct ScimFilter
   {
      ScimFilter() : present(false), boolean(false), flag(false) { }

      bool present;
      std::string attribute;
      std::string op;
      String value;
      bool boolean;
      bool flag;
   };

   bool ScimParseFilter(const std::string &text, ScimFilter &filter)
   {
      filter = ScimFilter();

      const std::string trimmed = ScimTrim(text);
      if (trimmed.empty())
         return true;

      filter.present = true;

      const size_t space = trimmed.find(' ');
      if (space == std::string::npos)
         return false;

      filter.attribute = ScimLower(trimmed.substr(0, space));

      const std::string rest = ScimTrim(trimmed.substr(space + 1));
      const size_t second = rest.find(' ');
      if (second == std::string::npos)
         return false;

      filter.op = ScimLower(rest.substr(0, second));

      // The attribute may be written fully qualified.
      if (ScimStartsWith(filter.attribute, ScimUserPathPrefix))
         filter.attribute = filter.attribute.substr(strlen(ScimUserPathPrefix));
      else if (ScimStartsWith(filter.attribute, ScimGroupPathPrefix))
         filter.attribute = filter.attribute.substr(strlen(ScimGroupPathPrefix));

      const std::string operand = ScimTrim(rest.substr(second + 1));
      const std::string loweredOperand = ScimLower(operand);
      if (loweredOperand == "true" || loweredOperand == "false")
      {
         filter.boolean = true;
         filter.flag = loweredOperand == "true";
         return true;
      }

      // A quoted string, with JSON's escapes: the parser that reads the bodies
      // reads this too, so "a\"b" is what it is everywhere.
      JsonValue value;
      std::string parseError;
      if (!JsonValue::Parse(operand, value, parseError) || !value.IsString())
         return false;

      if (value.AsString().find('\0') != std::string::npos)
         return false;

      filter.value = ScimWide(value.AsString());
      return true;
   }

   // The filters a listing of users answers (docs/Scim.md): userName eq, co and
   // sw, externalId eq, emails.value eq and active eq.
   bool ScimUserFilterAnswered(const ScimFilter &filter)
   {
      if (!filter.present)
         return true;

      if (filter.attribute == "username")
         return !filter.boolean && (filter.op == "eq" || filter.op == "co" || filter.op == "sw");

      if (filter.attribute == "externalid" || filter.attribute == "emails.value" || filter.attribute == "emails")
         return !filter.boolean && filter.op == "eq";

      if (filter.attribute == "active")
         return filter.boolean && filter.op == "eq";

      return false;
   }

   // And a listing of groups: displayName eq.
   bool ScimGroupFilterAnswered(const ScimFilter &filter)
   {
      if (!filter.present)
         return true;

      return filter.attribute == "displayname" && filter.op == "eq" && !filter.boolean;
   }

   // attribute[filter].sub, attribute.sub or attribute - lower-case already.
   bool ScimSplitPath(const std::string &path, std::string &head, bool &filtered, ScimFilter &filter, std::string &sub)
   {
      filtered = false;
      sub.clear();
      head.clear();

      const size_t open = path.find('[');
      if (open == std::string::npos)
      {
         const size_t dot = path.find('.');
         head = path.substr(0, dot);
         if (dot != std::string::npos)
            sub = path.substr(dot + 1);
         return !head.empty();
      }

      const size_t close = path.find(']', open);
      if (close == std::string::npos)
         return false;

      head = path.substr(0, open);
      if (!ScimParseFilter(path.substr(open + 1, close - open - 1), filter) || !filter.present)
         return false;

      filtered = true;

      const std::string rest = path.substr(close + 1);
      if (!rest.empty())
      {
         if (rest[0] != '.')
            return false;
         sub = rest.substr(1);
      }

      return !head.empty();
   }

   // ------------------------------------------------------------------ responses

   HttpResponse ScimRespond(int status, const AnsiString &body, const AnsiString &extraHeaders)
   {
      // application/scim+json, which BuildResponse_ does not write, and the
      // listener's own rule about caches: nothing answered here may be kept.
      HttpResponse response;
      response.status = status;
      response.content_type = "application/scim+json";
      response.body = body;
      response.extra_headers = extraHeaders + "Cache-Control: no-store\r\n";
      return response;
   }

   AnsiString ScimErrorDocument(int status, const AnsiString &scimType, const String &detail)
   {
      AnsiString statusText;
      statusText.Format("%d", status);

      AnsiString body = AnsiString("{\"schemas\":[\"") + ScimErrorSchema + "\"],\"status\":\"" + statusText + "\"";
      if (!scimType.IsEmpty())
         body += ",\"scimType\":\"" + scimType + "\"";
      body += ",\"detail\":" + ScimQuote(detail) + "}";
      return body;
   }

   HttpResponse ScimRefused(const ScimRefusal &refusal)
   {
      return ScimRespond(refusal.status, ScimErrorDocument(refusal.status, refusal.scim_type, refusal.detail), "");
   }

   HttpResponse ScimError(int status, const char *scimType, const String &detail)
   {
      ScimRefusal refusal;
      ScimRefuse(refusal, status, scimType, detail);
      return ScimRefused(refusal);
   }

   HttpResponse ScimNotFound(const wchar_t *resourceType)
   {
      // One answer for a resource that is not there and one at a domain this
      // key does not reach.
      return ScimError(404, "", String(_T("no ")) + resourceType + _T(" with that id"));
   }

   HttpResponse ScimInvalidFilter(bool users)
   {
      return ScimError(400, "invalidFilter", users
         ? String(_T("the filter is not one this server answers: userName eq, co or sw; externalId eq; emails.value eq; active eq true or false (docs/Scim.md)"))
         : String(_T("the filter is not one this server answers: displayName eq (docs/Scim.md)")));
   }

   AnsiString ScimBaseUrl(const AnsiString &host, bool tls)
   {
      // The URL the client reached, from its own Host field; a field that is
      // not a host name, an address and a port is not used, and meta.location
      // is then the path alone.
      bool usable = !host.IsEmpty() && host.GetLength() <= 255;
      for (int i = 0; usable && i < host.GetLength(); i++)
      {
         const char c = host.GetAt(i);
         usable = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-' || c == ':' || c == '[' || c == ']';
      }

      if (!usable)
         return AnsiString("/scim/v2");

      return AnsiString(tls ? "https://" : "http://") + host + "/scim/v2";
   }

   AnsiString ScimLocation(const ScimContext &scim, const char *collection, const String &id)
   {
      return scim.base + "/" + collection + "/" + Dav::PercentEncodeSegment(ScimUtf8(id));
   }

   AnsiString ScimListResponse(int total, int start, int items, const AnsiString &resources)
   {
      AnsiString numbers;
      numbers.Format("\"totalResults\":%d,\"itemsPerPage\":%d,\"startIndex\":%d", total, items, start);

      return AnsiString("{\"schemas\":[\"") + ScimListResponseSchema + "\"]," + numbers + ",\"Resources\":[" + resources + "]}";
   }

   // A resource as its attributes, name by name, so that one representation is
   // both what is answered (projected by attributes and excludedAttributes) and
   // what its version is computed from (whole).
   typedef std::vector<std::pair<const char *, AnsiString> > ScimResource;

   struct ScimProjection
   {
      std::vector<std::string> only;        // attributes=
      std::vector<std::string> excluded;    // excludedAttributes=

      bool Wants(const char *name) const
      {
         // RFC 7644 section 3.4.2.5: these are always returned.
         if (ScimSameName(name, "schemas") || ScimSameName(name, "id") || ScimSameName(name, "meta"))
            return true;

         if (!only.empty())
         {
            for (const std::string &wanted : only)
            {
               if (ScimSameName(wanted, name))
                  return true;
            }

            return false;
         }

         for (const std::string &left : excluded)
         {
            if (ScimSameName(left, name))
               return false;
         }

         return true;
      }
   };

   // A comma-separated list of attribute names, each reduced to the attribute it
   // is under: name.givenName asks for name, which is answered whole.
   void ScimReadAttributeList(const AnsiString &text, std::vector<std::string> &names)
   {
      std::string rest(text.c_str());
      while (!rest.empty())
      {
         const size_t comma = rest.find(',');
         std::string piece = ScimLower(ScimTrim(rest.substr(0, comma)));
         rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);

         if (ScimStartsWith(piece, ScimUserPathPrefix))
            piece = piece.substr(strlen(ScimUserPathPrefix));
         else if (ScimStartsWith(piece, ScimGroupPathPrefix))
            piece = piece.substr(strlen(ScimGroupPathPrefix));

         const size_t dot = piece.find('.');
         piece = piece.substr(0, dot);
         if (!piece.empty())
            names.push_back(piece);
      }
   }

   AnsiString ScimDocument(const ScimResource &resource, const ScimProjection *projection, const AnsiString &meta)
   {
      AnsiString json = "{";
      bool first = true;

      for (const std::pair<const char *, AnsiString> &member : resource)
      {
         if (projection && !projection->Wants(member.first))
            continue;

         if (!first)
            json += ",";
         first = false;

         json += AnsiString("\"") + member.first + "\":" + member.second;
      }

      if (!meta.IsEmpty())
      {
         if (!first)
            json += ",";
         json += "\"meta\":" + meta;
      }

      json += "}";
      return json;
   }

   // A weak ETag of the representation this server answers, whole and without
   // meta. hm_accounts and hm_distributionlists keep no time of change, so there
   // is no lastModified to take one from; this changes exactly when what a
   // client would read changes.
   AnsiString ScimVersion(const ScimResource &resource)
   {
      return "W/\"" + Dav::Sha256Hex(ScimDocument(resource, nullptr, AnsiString())).Left(16) + "\"";
   }

   AnsiString ScimMeta(const char *resourceType, const AnsiString &location, const AnsiString &version)
   {
      return AnsiString("{\"resourceType\":\"") + resourceType + "\",\"location\":\"" + ScimAsciiEscape(location) +
             "\",\"version\":\"" + ScimAsciiEscape(version) + "\"}";
   }

   // If-Match: a list of entity tags, or *. A weak tag and a strong one of the
   // same opaque value are the same version here, since this server only ever
   // hands out weak ones.
   bool ScimVersionMatches(const AnsiString &ifMatch, const AnsiString &version)
   {
      const std::string header = ScimTrim(std::string(ifMatch.c_str()));
      if (header.empty() || header == "*")
         return true;

      std::string current(version.c_str());
      if (ScimStartsWith(current, "W/"))
         current = current.substr(2);

      std::string rest = header;
      while (!rest.empty())
      {
         const size_t comma = rest.find(',');
         std::string tag = ScimTrim(rest.substr(0, comma));
         rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);

         if (ScimStartsWith(tag, "W/"))
            tag = tag.substr(2);

         if (tag == current)
            return true;
      }

      return false;
   }

   HttpResponse ScimVersionRefused()
   {
      return ScimError(412, "", _T("the resource has changed since the version If-Match names; read it again and send the change against what it is now"));
   }

   // ------------------------------------------------------------------ the store

   std::shared_ptr<Domain> ScimHostedDomain(const String &domainName)
   {
      if (domainName.IsEmpty())
         return std::shared_ptr<Domain>();

      Domains domains;
      domains.Refresh();
      return domains.GetItemByName(domainName);
   }

   // An address this key may give an account, an alias or a list: well formed,
   // at a domain this server hosts, and at one the key reaches.
   bool ScimProvisionable(const ScimContext &scim, const String &address, std::shared_ptr<Domain> &domain)
   {
      if (address.IsEmpty() || !StringParser::IsValidEmailAddress(address))
         return false;

      const String domainName = StringParser::ExtractDomain(address);
      if (!scim.Reaches(domainName))
         return false;

      domain = ScimHostedDomain(domainName);
      return domain ? true : false;
   }

   // The id of the account at an address, or 0. PersistentAccount's own lookup,
   // the one every sign-in makes: the address column's index on SQL Server and
   // MySQL, whose collations compare without regard to case, and lower() on both
   // sides on PostgreSQL, whose equality does not.
   __int64 ScimFindAccountId(const String &address)
   {
      if (address.IsEmpty() || address.GetLength() > 255)
         return 0;

      std::shared_ptr<Account> account(new Account());
      if (!PersistentAccount::ReadObject(account, address))
         return 0;

      return account->GetID();
   }

   // The alias at a name, by PersistentAlias's own lookup - the one RCPT TO
   // makes, compared as the account lookup above is.
   std::shared_ptr<Alias> ScimAliasNamed(const String &name)
   {
      if (name.IsEmpty() || name.GetLength() > 255)
         return std::shared_ptr<Alias>();

      std::shared_ptr<Alias> alias(new Alias());
      if (!PersistentAlias::ReadObject(alias, name) || alias->GetID() == 0)
         return std::shared_ptr<Alias>();

      return alias;
   }

   std::shared_ptr<Account> ScimAccountAt(const ScimContext &scim, const String &address)
   {
      if (!scim.Reaches(StringParser::ExtractDomain(address)))
         return std::shared_ptr<Account>();

      const __int64 id = ScimFindAccountId(address);
      if (id == 0)
         return std::shared_ptr<Account>();

      std::shared_ptr<Account> account(new Account());
      if (!PersistentAccount::ReadObject(account, id) || account->GetID() == 0)
         return std::shared_ptr<Account>();

      return account;
   }

   std::shared_ptr<DistributionList> ScimListAt(const ScimContext &scim, const String &address)
   {
      const String domainName = StringParser::ExtractDomain(address);
      if (address.IsEmpty() || !scim.Reaches(domainName))
         return std::shared_ptr<DistributionList>();

      std::shared_ptr<Domain> domain = ScimHostedDomain(domainName);
      if (!domain)
         return std::shared_ptr<DistributionList>();

      DistributionLists lists(domain->GetID());
      lists.Refresh();
      return lists.GetItemByAddress(address);
   }

   // Aliases, as rows: each read whole from the recordset, in the domains the
   // key reaches.
   bool ScimReadAliasRows(const ScimContext &scim, const SQLCommand &command, std::vector<std::shared_ptr<Alias> > &aliases)
   {
      aliases.clear();

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         std::shared_ptr<Alias> alias(new Alias());
         if (PersistentAlias::ReadObject(alias, recordset) && scim.Reaches(StringParser::ExtractDomain(alias->GetName())))
            aliases.push_back(alias);

         recordset->MoveNext();
      }

      return true;
   }

   // The aliases whose value is the address, in the domains the key reaches:
   // what emails answers beside the address, and all that a write of emails
   // may change. An alias in a domain the key does not reach is neither shown
   // nor touched. No index covers hm_aliases.aliasvalue on any backend, so
   // this reads the table through; a page of the listing reads it once for
   // every account on the page (ScimReadEveryAlias).
   bool ScimReadAliases(const ScimContext &scim, const String &address, std::vector<std::shared_ptr<Alias> > &aliases)
   {
      String lowered = address;
      lowered.MakeLower();

      SQLCommand command("select * from hm_aliases where lower(aliasvalue) = @VALUE order by aliasname");
      command.AddParameter("@VALUE", lowered);

      return ScimReadAliasRows(scim, command, aliases);
   }

   bool ScimReadEveryAlias(const ScimContext &scim, std::vector<std::shared_ptr<Alias> > &aliases)
   {
      SQLCommand command("select * from hm_aliases order by aliasname");
      return ScimReadAliasRows(scim, command, aliases);
   }

   // Of every alias read, the ones whose value is the address, in the order read.
   std::vector<std::shared_ptr<Alias> > ScimAliasesOf(const std::vector<std::shared_ptr<Alias> > &every, const String &address)
   {
      std::vector<std::shared_ptr<Alias> > aliases;
      for (const std::shared_ptr<Alias> &alias : every)
      {
         if (alias->GetValue().CompareNoCase(address) == 0)
            aliases.push_back(alias);
      }

      return aliases;
   }

   // Among these aliases, the one at the name given, or none.
   std::shared_ptr<Alias> ScimAliasAmong(const std::vector<std::shared_ptr<Alias> > &aliases, const String &name)
   {
      for (const std::shared_ptr<Alias> &alias : aliases)
      {
         if (alias->GetName().CompareNoCase(name) == 0)
            return alias;
      }

      return std::shared_ptr<Alias>();
   }

   // One row of the account listing, before the account is read whole.
   struct ScimCandidate
   {
      ScimCandidate() : id(0), active(false) { }

      __int64 id;
      String address;
      bool active;
   };

   bool ScimAllAccounts(std::vector<ScimCandidate> &candidates)
   {
      SQLCommand command("select accountid, accountaddress, accountactive from hm_accounts order by accountid");

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         ScimCandidate candidate;
         candidate.id = recordset->GetLongValue("accountid");
         candidate.address = recordset->GetStringValue("accountaddress");
         candidate.active = recordset->GetLongValue("accountactive") != 0;
         candidates.push_back(candidate);
         recordset->MoveNext();
      }

      return true;
   }

   bool ScimAccountsByExternalId(const String &externalId, std::vector<ScimCandidate> &candidates)
   {
      if (externalId.IsEmpty() || externalId.GetLength() > ScimMaxExternalId)
         return true;

      SQLCommand command("select accountid, accountaddress, accountactive, accountexternalid from hm_accounts where accountexternalid = @EXTERNALID order by accountid");
      command.AddParameter("@EXTERNALID", externalId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         // Compared again here, exactly: the column's collation is the
         // backend's, and externalId is case-exact in SCIM.
         if (recordset->GetStringValue("accountexternalid") == externalId)
         {
            ScimCandidate candidate;
            candidate.id = recordset->GetLongValue("accountid");
            candidate.address = recordset->GetStringValue("accountaddress");
            candidate.active = recordset->GetLongValue("accountactive") != 0;
            candidates.push_back(candidate);
         }

         recordset->MoveNext();
      }

      return true;
   }

   // The members of a list as SCIM answers them: the rows whose owner is a
   // member (DistributionListRecipient::GetIsMember), as ListEntryJson_ answers
   // them. A managed list's requests still waiting and the addresses that left
   // are rows too, and are neither shown nor changed here.
   std::vector<String> ScimMemberAddresses(const DistributionList &list)
   {
      std::vector<String> addresses;

      std::shared_ptr<DistributionListRecipients> recipients = list.GetMembers();
      if (!recipients)
         return addresses;

      for (int index = 0; index < recipients->GetCount(); index++)
      {
         std::shared_ptr<DistributionListRecipient> recipient = recipients->GetItem(index);
         if (recipient && recipient->GetIsMember())
            addresses.push_back(recipient->GetAddress());
      }

      return addresses;
   }

   // A member added as an administrator adds one: a confirmed row, receiving
   // each message, with no token until the list manager first needs one. A row
   // already there for the address - a request still waiting, an address that
   // left - is confirmed rather than doubled.
   bool ScimAddMember(__int64 listId, const String &address, ScimRefusal &refusal)
   {
      std::shared_ptr<DistributionListRecipient> row(new DistributionListRecipient());
      if (PersistentDistributionListRecipient::ReadByAddress(row, listId, address))
      {
         if (row->GetIsMember())
            return true;

         row->SetConfirmed(DistributionListRecipient::Confirmed);
         row->SetDelivery(DistributionListRecipient::DeliveryEach);
         row->SetSubscribed((__int64) time(nullptr));
         row->SetToken(String());

         if (!PersistentDistributionListRecipient::SaveState(row))
            return ScimRefuse(refusal, 500, "", _T("the member ") + address + _T(" could not be saved"));

         return true;
      }

      std::shared_ptr<DistributionListRecipient> added(new DistributionListRecipient());
      added->SetListID(listId);
      added->SetAddress(address);

      String saveError;
      if (!PersistentDistributionListRecipient::SaveObject(added, saveError, PersistenceModeNormal))
      {
         if (saveError.IsEmpty())
            return ScimRefuse(refusal, 500, "", _T("the member ") + address + _T(" could not be saved"));

         return ScimRefuse(refusal, 400, "invalidValue", saveError);
      }

      return true;
   }

   // A member removed: its row deleted. An address that is no member - that has
   // no row, or a row waiting or left - is left as it is.
   bool ScimRemoveMember(__int64 listId, const String &address, ScimRefusal &refusal)
   {
      std::shared_ptr<DistributionListRecipient> row(new DistributionListRecipient());
      if (!PersistentDistributionListRecipient::ReadByAddress(row, listId, address) || !row->GetIsMember())
         return true;

      if (!PersistentDistributionListRecipient::DeleteObject(row))
         return ScimRefuse(refusal, 500, "", _T("the member ") + address + _T(" could not be removed"));

      return true;
   }

   // Every row of every distribution list that names the address - a member, a
   // request still waiting, a departure recorded - deleted through the
   // recipient's own persistence, which the audit trail records. Rows of the
   // lists' tables only: a membership query is a rule, and names nobody. What a
   // removed account would otherwise leave in a list is a member nobody is,
   // whose copy of every message is refused, and whose memberships a later
   // account at the same address would inherit. failed names the lists whose
   // row could not be removed.
   bool ScimRemoveFromLists(const String &address, int &removed, String &failed)
   {
      removed = 0;

      String lowered = address;
      lowered.MakeLower();

      SQLCommand command("select * from hm_distributionlistsrecipients where lower(distributionlistrecipientaddress) = @ADDRESS");
      command.AddParameter("@ADDRESS", lowered);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
      {
         failed = _T("the distribution lists, which could not be read");
         return false;
      }

      std::vector<std::shared_ptr<DistributionListRecipient> > rows;
      while (!recordset->IsEOF())
      {
         std::shared_ptr<DistributionListRecipient> row(new DistributionListRecipient());
         if (PersistentDistributionListRecipient::ReadObject(row, recordset))
            rows.push_back(row);

         recordset->MoveNext();
      }

      bool whole = true;
      for (const std::shared_ptr<DistributionListRecipient> &row : rows)
      {
         if (PersistentDistributionListRecipient::DeleteObject(row))
         {
            removed++;
            continue;
         }

         std::shared_ptr<DistributionList> list(new DistributionList());
         String name;
         if (PersistentDistributionList::ReadObject(list, (__int64) row->GetListID()))
            name = list->GetAddress();
         else
            name.Format(_T("list %I64d"), (__int64) row->GetListID());

         failed += (failed.IsEmpty() ? String() : String(_T(", "))) + name;
         whole = false;
      }

      return whole;
   }

   // A suspension, a new password and a removal all end the person's sessions:
   // every browser session ended, and every live IMAP, POP3 and SMTP connection
   // the sign-in list registers dropped where it stands. The account's own
   // switch refuses every sign-in after a suspension, and the new password every
   // sign-in with the old one.
   void ScimEndSessions(__int64 accountId)
   {
      RestApiServer::RevokeSessionsForAccount(accountId);
      SignInStore::DropConnectionsForAccount(accountId);
   }

   // ------------------------------------------------------------------ users

   struct ScimUserRecord
   {
      std::shared_ptr<Account> account;
      std::vector<std::shared_ptr<Alias> > aliases;
   };

   bool ScimLoadUser(const ScimContext &scim, const String &id, ScimUserRecord &user)
   {
      user.account = ScimAccountAt(scim, id);
      if (!user.account)
         return false;

      ScimReadAliases(scim, user.account->GetAddress(), user.aliases);
      return true;
   }

   String ScimDisplayName(const Account &account)
   {
      String name = account.GetPersonFirstName();
      const String last = account.GetPersonLastName();

      if (!last.IsEmpty())
      {
         if (!name.IsEmpty())
            name += _T(" ");
         name += last;
      }

      return name;
   }

   ScimResource ScimUserResource(const ScimUserRecord &user)
   {
      const Account &account = *user.account;
      const String address = account.GetAddress();
      const String first = account.GetPersonFirstName();
      const String last = account.GetPersonLastName();
      const String display = ScimDisplayName(account);

      ScimResource resource;
      resource.push_back(std::make_pair("schemas", AnsiString("[\"") + ScimUserSchema + "\"]"));
      resource.push_back(std::make_pair("id", ScimQuote(address)));

      if (!account.GetExternalId().IsEmpty())
         resource.push_back(std::make_pair("externalId", ScimQuote(account.GetExternalId())));

      resource.push_back(std::make_pair("userName", ScimQuote(address)));

      if (!display.IsEmpty())
      {
         AnsiString name = "{";
         if (!first.IsEmpty())
            name += "\"givenName\":" + ScimQuote(first) + ",";
         if (!last.IsEmpty())
            name += "\"familyName\":" + ScimQuote(last) + ",";
         name += "\"formatted\":" + ScimQuote(display) + "}";

         resource.push_back(std::make_pair("name", name));
         resource.push_back(std::make_pair("displayName", ScimQuote(display)));
      }

      resource.push_back(std::make_pair("active", AnsiString(account.GetActive() ? "true" : "false")));

      // The address is the work one and the primary; an alias is other, never
      // primary - the old address a rename kept among them.
      AnsiString emails = "[{\"value\":" + ScimQuote(address) + ",\"type\":\"work\",\"primary\":true}";
      for (const std::shared_ptr<Alias> &alias : user.aliases)
         emails += ",{\"value\":" + ScimQuote(alias->GetName()) + ",\"type\":\"other\",\"primary\":false}";
      emails += "]";

      resource.push_back(std::make_pair("emails", emails));
      return resource;
   }

   HttpResponse ScimUserAnswer(const ScimContext &scim, int status, const ScimUserRecord &user, const ScimProjection *projection, bool created)
   {
      const ScimResource resource = ScimUserResource(user);
      const AnsiString version = ScimVersion(resource);
      const AnsiString location = ScimLocation(scim, "Users", user.account->GetAddress());

      AnsiString headers = "ETag: " + version + "\r\n";
      if (created)
         headers += "Location: " + location + "\r\n";

      return ScimRespond(status, ScimDocument(resource, projection, ScimMeta("User", location, version)), headers);
   }

   // A change to an account, as it will be once written: read from the account
   // and then from the request, so that a PUT and a PATCH are decided on one
   // shape and written by one function.
   struct ScimUserChange
   {
      ScimUserChange() : active(true), password_given(false), name_given(false), display_given(false) { }

      String address;                      // userName, as it will be
      String first;
      String last;
      bool active;
      String external_id;
      bool password_given;
      String password;
      std::vector<String> aliases;         // the aliases, as they will be
      std::vector<String> work_claims;     // what the request said the work or primary address is
      bool name_given;                     // a name, givenName or familyName was written
      bool display_given;                  // a displayName was written
      String display_name;
   };

   ScimUserChange ScimCurrentState(const ScimUserRecord &user)
   {
      ScimUserChange change;
      change.address = user.account->GetAddress();
      change.first = user.account->GetPersonFirstName();
      change.last = user.account->GetPersonLastName();
      change.active = user.account->GetActive();
      change.external_id = user.account->GetExternalId();

      for (const std::shared_ptr<Alias> &alias : user.aliases)
         change.aliases.push_back(alias->GetName());

      return change;
   }

   // One entry of emails: {value, type, primary, display}.
   bool ScimReadEmail(const JsonValue &entry, String &value, String &type, bool &primary)
   {
      value = String();
      type = String();
      primary = false;

      if (!entry.IsObject())
         return false;

      const JsonValue *valueMember = ScimAttr(entry, "value");
      if (!valueMember || !ScimReadText(*valueMember, value))
         return false;

      value.Trim();

      const JsonValue *typeMember = ScimAttr(entry, "type");
      if (typeMember && !typeMember->IsNull() && !ScimReadText(*typeMember, type))
         return false;

      const JsonValue *primaryMember = ScimAttr(entry, "primary");
      if (primaryMember && !primaryMember->IsNull() && !ScimReadFlag(*primaryMember, primary))
         return false;

      return !value.IsEmpty();
   }

   // An entry of emails, sorted: the work or primary one is the account's own
   // address, judged once the request's userName is known; any other is an
   // alias.
   bool ScimTakeEmail(const JsonValue &entry, ScimUserChange &change, ScimRefusal &refusal)
   {
      String value;
      String type;
      bool primary = false;

      if (!ScimReadEmail(entry, value, type, primary))
         return ScimRefuse(refusal, 400, "invalidValue", _T("each entry of emails is an object with value, the address, and optionally type and primary"));

      if (primary || type.CompareNoCase(_T("work")) == 0)
      {
         change.work_claims.push_back(value);
         return true;
      }

      if (!ScimContainsNoCase(change.aliases, value))
         change.aliases.push_back(value);

      return true;
   }

   bool ScimTakeEmails(const JsonValue &value, ScimUserChange &change, ScimRefusal &refusal)
   {
      // One object is taken as a list of one, as some clients add that way.
      if (value.IsObject())
         return ScimTakeEmail(value, change, refusal);

      if (!value.IsArray())
         return ScimRefuse(refusal, 400, "invalidValue", _T("emails is an array of {value, type, primary}"));

      for (const JsonValue &entry : value.Items())
      {
         if (!ScimTakeEmail(entry, change, refusal))
            return false;
      }

      return true;
   }

   // name: {givenName, familyName}. whole says the value replaces the whole of
   // name, as a PUT's does, so that a sub-attribute it leaves out is cleared;
   // a PATCH's leaves one it does not name as it was (RFC 7644 section
   // 3.5.2.3).
   bool ScimTakeName(const JsonValue &value, ScimUserChange &change, bool whole, ScimRefusal &refusal)
   {
      if (!value.IsObject())
         return ScimRefuse(refusal, 400, "invalidValue", _T("name is an object of givenName and familyName"));

      if (whole)
      {
         change.first = String();
         change.last = String();
      }

      change.name_given = true;

      for (const std::pair<std::string, JsonValue> &member : value.Members())
      {
         const bool given = ScimSameName(member.first, "givenName");
         const bool family = ScimSameName(member.first, "familyName");

         if (given || family)
         {
            String text;
            if (!member.second.IsNull() && !ScimReadText(member.second, text))
               return ScimRefuse(refusal, 400, "invalidValue", _T("name.givenName and name.familyName are strings"));

            if (given)
               change.first = text;
            else
               change.last = text;

            continue;
         }

         // Composed from the two, or not kept.
         if (ScimSameName(member.first, "formatted") || ScimSameName(member.first, "middleName") ||
             ScimSameName(member.first, "honorificPrefix") || ScimSameName(member.first, "honorificSuffix"))
            continue;

         return ScimRefuse(refusal, 400, "invalidValue", _T("name.") + ScimWide(member.first) + _T(" is not a part of name"));
      }

      return true;
   }

   // A displayName written with no name beside it is split at its last space:
   // the account keeps a first and a last name, and nothing else of a name.
   void ScimSplitDisplayName(const String &display, ScimUserChange &change)
   {
      String text = display;
      text.Trim();

      const int space = text.ReverseFind(_T(" "));
      if (space < 0)
      {
         change.first = text;
         change.last = String();
         return;
      }

      change.first = text.Left(space);
      change.first.Trim();
      change.last = text.Mid(space + 1);
      change.last.Trim();
   }

   // What a POST or a PUT body says, onto change. Every attribute of the User
   // schema this server keeps is read; one it does not keep, a read-only one and
   // an extension's are accepted and not kept; anything else is refused by name,
   // which is what catches a misspelling. An attribute the body leaves out is
   // not asserted and stays as it is (RFC 7644 section 3.5.1 leaves the choice
   // to the server), emails included: a PUT without emails leaves the aliases.
   bool ScimReadUserBody(const JsonValue &body, ScimUserChange &change, ScimRefusal &refusal)
   {
      const JsonValue *userName = ScimAttr(body, "userName");
      if (!userName || !ScimReadText(*userName, change.address))
         return ScimRefuse(refusal, 400, "invalidValue", _T("userName is required: the account's address"));

      for (const std::pair<std::string, JsonValue> &member : body.Members())
      {
         const std::string &key = member.first;
         const JsonValue &value = member.second;

         if (ScimSameName(key, "userName"))
            continue;

         if (ScimSameName(key, "name"))
         {
            if (value.IsNull())
            {
               change.first = String();
               change.last = String();
               change.name_given = true;
            }
            else if (!ScimTakeName(value, change, true, refusal))
            {
               return false;
            }

            continue;
         }

         if (ScimSameName(key, "displayName"))
         {
            String text;
            if (!value.IsNull() && !ScimReadText(value, text))
               return ScimRefuse(refusal, 400, "invalidValue", _T("displayName is a string"));

            change.display_given = true;
            change.display_name = text;
            continue;
         }

         if (ScimSameName(key, "active"))
         {
            if (!value.IsNull() && !ScimReadFlag(value, change.active))
               return ScimRefuse(refusal, 400, "invalidValue", _T("active is true or false"));
            continue;
         }

         if (ScimSameName(key, "externalId"))
         {
            change.external_id = String();
            if (!value.IsNull() && !ScimReadText(value, change.external_id))
               return ScimRefuse(refusal, 400, "invalidValue", _T("externalId is a string"));
            continue;
         }

         if (ScimSameName(key, "password"))
         {
            if (value.IsNull())
               continue;

            if (!ScimReadText(value, change.password))
               return ScimRefuse(refusal, 400, "invalidValue", _T("password is a string"));

            change.password_given = true;
            continue;
         }

         if (ScimSameName(key, "emails"))
         {
            change.aliases.clear();
            if (!value.IsNull() && !ScimTakeEmails(value, change, refusal))
               return false;
            continue;
         }

         if (ScimIgnoredUserAttribute(key))
            continue;

         return ScimRefuse(refusal, 400, "invalidValue", ScimWide(key.substr(0, 64)) + _T(" is not an attribute of the SCIM User schema (docs/Scim.md)"));
      }

      return true;
   }

   // The emails operations. A filter on type names the work address (the
   // account's own, which may be written as itself and never removed) or any
   // other (the aliases); a filter on value names one address, for remove.
   bool ScimApplyEmailsOperation(const std::string &op, bool filtered, const ScimFilter &filter, const std::string &sub,
                                 const JsonValue *value, ScimUserChange &change, ScimRefusal &refusal)
   {
      const String ownAddressSentence = _T("the account's own address cannot be removed from emails; an alias is removed by emails[value eq \"...\"]");

      if (!filtered)
      {
         if (!sub.empty())
            return ScimRefuse(refusal, 400, "invalidPath", _T("emails is written whole, or one entry by emails[type eq \"...\"] or emails[value eq \"...\"]"));

         if (op == "remove")
            return ScimRefuse(refusal, 400, "invalidValue", ownAddressSentence);

         if (!value || value->IsNull())
            return ScimRefuse(refusal, 400, "invalidValue", _T("add and replace of emails carry the entries"));

         if (op == "replace")
            change.aliases.clear();

         return ScimTakeEmails(*value, change, refusal);
      }

      if (filter.op != "eq")
         return ScimRefuse(refusal, 400, "invalidPath", _T("a filter in an emails path compares with eq"));

      // emails[value eq "..."]: one address, for remove.
      if (filter.attribute == "value")
      {
         if (op != "remove" || !sub.empty() || filter.boolean)
            return ScimRefuse(refusal, 400, "invalidPath", _T("emails[value eq \"...\"] is for remove; an address is added to emails with add"));

         if (filter.value.CompareNoCase(change.address) == 0)
            return ScimRefuse(refusal, 400, "invalidValue", ownAddressSentence);

         if (!ScimContainsNoCase(change.aliases, filter.value))
            return ScimRefuse(refusal, 400, "noTarget", _T("no address in emails is ") + filter.value);

         ScimRemoveNoCase(change.aliases, filter.value);
         return true;
      }

      // emails[type eq "work"] or emails[primary eq true] is the account's own
      // address; any other type is an alias.
      bool work = false;
      if (filter.attribute == "type" && !filter.boolean)
         work = filter.value.CompareNoCase(_T("work")) == 0;
      else if (filter.attribute == "primary" && filter.boolean)
         work = filter.flag;
      else
         return ScimRefuse(refusal, 400, "invalidPath", _T("an emails path is filtered on type, primary or value"));

      // A sub-attribute that is not the address itself is the server's to set:
      // the work address is primary, an alias is not.
      if (sub == "primary" || sub == "type" || sub == "display")
         return true;

      if (!sub.empty() && sub != "value")
         return ScimRefuse(refusal, 400, "invalidPath", _T("an entry of emails has value, type and primary"));

      if (op == "remove")
      {
         if (work)
            return ScimRefuse(refusal, 400, "invalidValue", ownAddressSentence);

         change.aliases.clear();
         return true;
      }

      if (!value || value->IsNull())
         return ScimRefuse(refusal, 400, "invalidValue", _T("add and replace of emails carry the address"));

      String address;
      if (sub == "value")
      {
         if (!ScimReadText(*value, address))
            return ScimRefuse(refusal, 400, "invalidValue", _T("the value of an address in emails is a string"));
      }
      else
      {
         String type;
         bool primary = false;
         if (!ScimReadEmail(*value, address, type, primary))
            return ScimRefuse(refusal, 400, "invalidValue", _T("each entry of emails is an object with value, the address, and optionally type and primary"));
      }

      address.Trim();

      if (work)
      {
         change.work_claims.push_back(address);
         return true;
      }

      if (op == "replace")
         change.aliases.clear();

      if (!ScimContainsNoCase(change.aliases, address))
         change.aliases.push_back(address);

      return true;
   }

   // One PATCH operation on a user, onto change. op is add, replace or remove,
   // lower-case; path as the request wrote it, or empty.
   bool ScimApplyUserOperation(const std::string &op, const std::string &path, const JsonValue *value, ScimUserChange &change, ScimRefusal &refusal)
   {
      std::string lowered = ScimLower(ScimTrim(path));

      // No path: the value is an object of attributes and their values, each an
      // operation of its own - which is how Entra sends a replace, with keys
      // such as "name.givenName" and "emails[type eq \"work\"].value".
      if (lowered.empty())
      {
         if (op == "remove")
            return ScimRefuse(refusal, 400, "noTarget", _T("remove needs a path"));

         if (!value || !value->IsObject())
            return ScimRefuse(refusal, 400, "invalidValue", _T("an operation with no path carries an object of attributes and their values"));

         for (const std::pair<std::string, JsonValue> &member : value->Members())
         {
            if (!ScimApplyUserOperation(op, member.first, &member.second, change, refusal))
               return false;
         }

         return true;
      }

      if (ScimStartsWith(lowered, ScimUserPathPrefix))
         lowered = lowered.substr(strlen(ScimUserPathPrefix));
      else if (ScimStartsWith(lowered, "urn:"))
         return true;   // an extension's attribute: not kept

      std::string head;
      std::string sub;
      bool filtered = false;
      ScimFilter filter;

      const String pathNamesNothing = _T("the path ") + ScimWide(ScimTrim(path).substr(0, 96)) + _T(" names no attribute of the SCIM User schema this server reads (docs/Scim.md)");

      if (!ScimSplitPath(lowered, head, filtered, filter, sub))
         return ScimRefuse(refusal, 400, "invalidPath", pathNamesNothing);

      const bool removing = op == "remove" || (value && value->IsNull());

      if (head == "emails")
         return ScimApplyEmailsOperation(op, filtered, filter, sub, value, change, refusal);

      if (ScimIgnoredUserAttribute(head))
         return true;

      if (filtered)
         return ScimRefuse(refusal, 400, "invalidPath", pathNamesNothing);

      if (head == "name")
      {
         if (sub.empty())
         {
            if (removing)
            {
               change.first = String();
               change.last = String();
               change.name_given = true;
               return true;
            }

            return ScimTakeName(*value, change, false, refusal);
         }

         if (sub == "givenname" || sub == "familyname")
         {
            String text;
            if (!removing && !ScimReadText(*value, text))
               return ScimRefuse(refusal, 400, "invalidValue", _T("name.givenName and name.familyName are strings"));

            if (sub == "givenname")
               change.first = text;
            else
               change.last = text;

            change.name_given = true;
            return true;
         }

         if (sub == "formatted" || sub == "middlename" || sub == "honorificprefix" || sub == "honorificsuffix")
            return true;

         return ScimRefuse(refusal, 400, "invalidPath", pathNamesNothing);
      }

      if (!sub.empty())
         return ScimRefuse(refusal, 400, "invalidPath", pathNamesNothing);

      if (head == "active")
      {
         if (op == "remove")
            return ScimRefuse(refusal, 400, "invalidValue", _T("active cannot be removed; replace it with true or false"));

         bool flag = false;
         if (!value || !ScimReadFlag(*value, flag))
            return ScimRefuse(refusal, 400, "invalidValue", _T("active is true or false"));

         change.active = flag;
         return true;
      }

      if (head == "username")
      {
         String text;
         if (removing || !ScimReadText(*value, text))
            return ScimRefuse(refusal, 400, "invalidValue", _T("userName is the account's address, and is replaced, never removed"));

         text.Trim();
         change.address = text;
         return true;
      }

      if (head == "externalid")
      {
         change.external_id = String();
         if (!removing && !ScimReadText(*value, change.external_id))
            return ScimRefuse(refusal, 400, "invalidValue", _T("externalId is a string"));
         return true;
      }

      if (head == "displayname")
      {
         String text;
         if (!removing && !ScimReadText(*value, text))
            return ScimRefuse(refusal, 400, "invalidValue", _T("displayName is a string"));

         change.display_given = true;
         change.display_name = text;
         return true;
      }

      if (head == "password")
      {
         String text;
         if (removing || !ScimReadText(*value, text))
            return ScimRefuse(refusal, 400, "invalidValue", _T("password is replaced with a string, and never removed"));

         change.password = text;
         change.password_given = true;
         return true;
      }

      return ScimRefuse(refusal, 400, "invalidPath", pathNamesNothing);
   }

   // The change made whole: the aliases trimmed, the account's own address and
   // the doubles taken out, and a displayName that came with no name split into
   // one - but only for an account that has no name yet: a POST, or a change to
   // an account made without one. An account's first and last names are what
   // its displayName is composed of, and they feed signatures, the directory
   // and the impersonation check; a displayName written alone to an account
   // that has them - Entra sends one when only the display name changed in the
   // directory - is accepted and not kept, rather than splitting "Ada Lovelace
   // (Finance)" into a first name and a last name nobody chose.
   void ScimFinishChange(ScimUserChange &change)
   {
      change.address.Trim();

      if (change.display_given && !change.name_given && change.first.IsEmpty() && change.last.IsEmpty())
         ScimSplitDisplayName(change.display_name, change);

      change.first.Trim();
      change.last.Trim();

      std::vector<String> aliases;
      for (const String &alias : change.aliases)
      {
         String trimmed = alias;
         trimmed.Trim();

         if (trimmed.IsEmpty() || trimmed.CompareNoCase(change.address) == 0 || ScimContainsNoCase(aliases, trimmed))
            continue;

         aliases.push_back(trimmed);
      }

      change.aliases.swap(aliases);
   }

   bool ScimValidateChange(const ScimUserChange &change, ScimRefusal &refusal)
   {
      if (change.address.IsEmpty() || !StringParser::IsValidEmailAddress(change.address))
         return ScimRefuse(refusal, 400, "invalidValue", ScimOutsideSentence(_T("userName")));

      if (change.first.GetLength() > ScimMaxPersonName || change.last.GetLength() > ScimMaxPersonName)
         return ScimRefuse(refusal, 400, "invalidValue", _T("name.givenName and name.familyName are at most 60 characters each"));

      if (change.external_id.GetLength() > ScimMaxExternalId || ScimHasLineBreak(change.external_id))
         return ScimRefuse(refusal, 400, "invalidValue", _T("externalId is at most 255 characters, on one line"));

      for (const String &claim : change.work_claims)
      {
         if (claim.CompareNoCase(change.address) != 0)
            return ScimRefuse(refusal, 400, "invalidValue",
               _T("the work address in emails, and the primary one, is the account's own address - userName, ") + change.address +
               _T(" - and ") + claim + _T(" is not; another address of the person's is an alias, of type other"));
      }

      if (change.password_given && change.password.IsEmpty())
         return ScimRefuse(refusal, 400, "invalidValue", _T("password must not be empty"));

      return true;
   }

   // An alias an identity provider asks for, judged as its save will judge it
   // (PreSaveLimitationsCheck) before anything of the change is written: at a
   // domain the key provisions, at an address nothing else has, and not one of
   // the domain's spam traps. The domain's alias limit is ScimJudgeAliasLimits',
   // since it depends on everything else the change writes.
   bool ScimCheckNewAlias(const ScimContext &scim, const String &name, ScimRefusal &refusal)
   {
      std::shared_ptr<Domain> domain;
      if (!ScimProvisionable(scim, name, domain))
         return ScimRefuse(refusal, 400, "invalidValue", _T("every address in emails must be at a domain this api key provisions, and ") + name + _T(" is not"));

      if (ScimFindAccountId(name) != 0)
         return ScimRefuse(refusal, 409, "uniqueness", name + _T(" is an account's address"));

      if (ScimAliasNamed(name))
         return ScimRefuse(refusal, 409, "uniqueness", name + _T(" is already an alias"));

      if (ScimListAt(scim, name))
         return ScimRefuse(refusal, 409, "uniqueness", name + _T(" is a distribution list's address"));

      if (SpamTrapMonitor::IsTrapAddress(name))
         return ScimRefuse(refusal, 400, "invalidValue", name + _T(" is one of its domain's spam traps. Remove the trap before giving the address to an account, an alias or a distribution list."));

      return true;
   }

   // What a change does to one domain's aliases, for its alias limit.
   struct ScimDomainAliases
   {
      ScimDomainAliases() : gave_way(0), kept(0), removed(0), created(0) { }

      String name;
      int gave_way;   // the alias a rename onto it deletes, first of all
      int kept;       // the old address the rename leaves there as an alias
      int removed;    // the aliases the change removes, after the rename
      int created;    // the aliases it makes, last
   };

   ScimDomainAliases &ScimAliasesIn(std::vector<ScimDomainAliases> &domains, const String &name)
   {
      for (ScimDomainAliases &domain : domains)
      {
         if (domain.name.CompareNoCase(name) == 0)
            return domain;
      }

      ScimDomainAliases added;
      added.name = name;
      domains.push_back(added);
      return domains.back();
   }

   // Each domain's alias limit, counted at the fullest point the writes reach,
   // in the order ScimCommitUser makes them: the rename's alias is left before
   // the change's removals, and the new aliases are made after them. A limit
   // met there would refuse the save half way through; met here, it refuses
   // the change before anything is written.
   bool ScimJudgeAliasLimits(const std::vector<ScimDomainAliases> &domains, ScimRefusal &refusal)
   {
      for (const ScimDomainAliases &counted : domains)
      {
         if (counted.kept == 0 && counted.created == 0)
            continue;

         std::shared_ptr<Domain> domain = ScimHostedDomain(counted.name);
         if (!domain || !domain->GetMaxNoOfAliasesEnabled())
            continue;

         const int limit = domain->GetMaxNoOfAliases();
         const int now = domain->GetAliases()->GetCount() - counted.gave_way;
         const int atRename = now + counted.kept;
         const int atEnd = atRename - counted.removed + counted.created;

         if ((counted.kept > 0 && atRename > limit) || (counted.created > 0 && atEnd > limit))
         {
            return ScimRefuse(refusal, 400, "invalidValue",
               _T("the domain ") + counted.name + _T(" holds the maximum number of aliases, and this change would make more there") +
               (counted.kept > 0 ? String(_T(" - a rename keeps the old address as an alias")) : String()));
         }
      }

      return true;
   }

   // A fresh Alias, as InterfaceAliases::Add and the REST alias route make one.
   // PersistentAlias::SaveObject asks the limitation check again, and evicts
   // the cache so that the next RCPT TO reads the row. made is the alias as
   // saved, with its id, so that a change that fails later can remove it.
   bool ScimCreateAlias(const String &name, const String &value, std::shared_ptr<Alias> &made, ScimRefusal &refusal)
   {
      std::shared_ptr<Domain> domain = ScimHostedDomain(StringParser::ExtractDomain(name));
      if (!domain)
         return ScimRefuse(refusal, 400, "invalidValue", _T("every address in emails must be at a domain this api key provisions, and ") + name + _T(" is not"));

      std::shared_ptr<Alias> alias(new Alias());
      alias->SetDomainID(domain->GetID());
      alias->SetName(name);
      alias->SetValue(value);
      alias->SetIsActive(true);

      String saveError;
      if (!PersistentAlias::SaveObject(alias, saveError, PersistenceModeNormal))
      {
         if (saveError.IsEmpty())
            return ScimRefuse(refusal, 500, "", _T("the alias ") + name + _T(" could not be saved"));

         const bool taken = saveError.Find(_T("already exists")) >= 0;
         return ScimRefuse(refusal, taken ? 409 : 400, taken ? "uniqueness" : "invalidValue", saveError);
      }

      made = alias;
      return true;
   }

   // An alias this change deleted, made again as it was, pointing at the
   // address the account is at now.
   bool ScimRestoreAlias(const std::shared_ptr<Alias> &gone, const String &value)
   {
      std::shared_ptr<Alias> alias(new Alias());
      alias->SetDomainID(gone->GetDomainID());
      alias->SetName(gone->GetName());
      alias->SetValue(value);
      alias->SetIsActive(gone->GetIsActive());

      String saveError;
      return PersistentAlias::SaveObject(alias, saveError, PersistenceModeNormal);
   }

   // Every place a password is chosen asks the same questions in the same order
   // (build/check-password-choice-sites.py): the policy, the reuse history of an
   // account that has one, and the published breach corpus. Here they are asked
   // before anything of the change is written. The new hash is set by
   // ScimSetPasswordHash as the account is written, and the old one goes into
   // the reuse history (PasswordHistory::Record) only once the whole change has
   // been - so a change that is refused, or put back, leaves no row there. A
   // refusal is 400 invalidValue with the sentence the policy or the check
   // wrote, or 503 when the breach check could not be asked and is set to
   // refuse.
   bool ScimJudgePassword(std::shared_ptr<Account> account, const String &password, bool isNew, ScimRefusal &refusal)
   {
      String policyFailure;
      if (!PasswordPolicy::IsAcceptable(account->GetAddress(), password, policyFailure))
      {
         LOG_APPLICATION(_T("REST API: SCIM refused a password for ") + account->GetAddress() + _T(": ") + policyFailure);
         return ScimRefuse(refusal, 400, "invalidValue", policyFailure);
      }

      if (!isNew && PasswordHistory::IsReuse(account, password))
         return ScimRefuse(refusal, 400, "invalidValue", _T("This password has been used recently on this account. Choose one that has not."));

      // A mailbox encrypted at rest (MailboxKeys.h) whose key is closed cannot
      // take a password nothing could wrap its key by.
      String keyRefusal;
      if (!isNew && !MailboxKeys::CanWrapNewPassword(account->GetID(), keyRefusal))
      {
         const int status = MailboxKeys::RefusalStatus(keyRefusal, 400);
         return ScimRefuse(refusal, status, status == 400 ? "mutability" : "", keyRefusal);
      }

      String breachRefusal;
      AnsiString breachReason;
      int breachStatus = 0;
      if (!PasswordBreachCheck::IsAcceptable(account->GetAddress(), password, breachRefusal, breachReason, breachStatus))
         return ScimRefuse(refusal, breachStatus, breachStatus == 400 ? "invalidValue" : "", breachRefusal);

      return true;
   }

   // The configured hash and the age clock, on the object the caller saves.
   void ScimSetPasswordHash(std::shared_ptr<Account> account, const String &password)
   {
      // A mailbox encrypted at rest: its key wrapped by the password, stored
      // when the account is saved (ScimJudgePassword asked whether it can be).
      String keyRefusal;
      MailboxKeys::PrepareForNewPassword(account, password, keyRefusal);

      const int preferredHashAlgorithm = IniFileSettings::Instance()->GetPreferredHashAlgorithm();
      account->SetPassword(Crypt::Instance()->EnCrypt(password, (Crypt::EncryptionType) preferredHashAlgorithm));
      account->SetPasswordEncryption(preferredHashAlgorithm);
      account->SetPasswordChanged(Time::GetCurrentDateTime());
   }

   // Thirty-two random bytes nobody is ever told, for an account made with no
   // password - Entra sends none. Until an administrator or the identity
   // provider sets one, the account is signed in to through single sign-on or
   // the directory and no other way. No policy runs on it, because nobody chose
   // it.
   bool ScimUnusablePassword(String &password)
   {
      unsigned char bytes[32];
      if (RAND_bytes(bytes, (int) sizeof(bytes)) != 1)
         return false;

      static const char *hex = "0123456789abcdef";
      AnsiString text;
      for (size_t i = 0; i < sizeof(bytes); i++)
      {
         text += hex[bytes[i] >> 4];
         text += hex[bytes[i] & 0x0f];
      }

      password = String(text);
      return true;
   }

   // The authority a key needs over an account it changes or removes:
   // RoleManager's rule for every interface - a credential may not take over an
   // account whose authority it has not got. For a key confined to named
   // domains that means every permission the account holds, in domains inside
   // the key's list: an account that administers another domain, or reaches
   // every domain - the server administrator, an auditor given no list - is not
   // its to change (RoleManager::Encompasses).
   bool ScimMayChange(const ScimContext &scim, std::shared_ptr<Account> account, ScimRefusal &refusal)
   {
      if (!RoleManager::Encompasses(scim.grant, RoleManager::GrantForAccount(account)))
         return ScimRefuse(refusal, 403, "", _T("that account holds authority this api key does not, so it is not this key's to change"));

      return true;
   }

   // What a change that failed after its first write had written, put back:
   // the aliases it made removed, the aliases it removed made again, and the
   // account's own columns - its names, its switch, its external id and its
   // password - as they were read. The address is not among them: a rename
   // that happened stays, and is reported. left names what could not be put
   // back.
   //
   // keyBefore, when the account's mailbox is encrypted at rest and the change
   // set a password: the key's wraps as they were before it, put back with the
   // password - the key wrapped only by the password being taken back would
   // otherwise open with nothing the account has (MailboxKeys::RestoreWraps).
   bool ScimPutBack(std::shared_ptr<Account> account, const Account &before,
                    const std::vector<std::shared_ptr<Alias> > &removed, const std::vector<std::shared_ptr<Alias> > &made,
                    const MailboxKeys::Row *keyBefore, String &left)
   {
      bool whole = true;

      for (const std::shared_ptr<Alias> &alias : made)
      {
         if (PersistentAlias::DeleteObject(alias))
            continue;

         left += (left.IsEmpty() ? String() : String(_T(", "))) + _T("the new alias ") + alias->GetName();
         whole = false;
      }

      for (const std::shared_ptr<Alias> &alias : removed)
      {
         if (ScimRestoreAlias(alias, account->GetAddress()))
            continue;

         left += (left.IsEmpty() ? String() : String(_T(", "))) + _T("the removed alias ") + alias->GetName();
         whole = false;
      }

      account->SetPersonFirstName(before.GetPersonFirstName());
      account->SetPersonLastName(before.GetPersonLastName());
      account->SetActive(before.GetActive());
      account->SetExternalId(before.GetExternalId());
      account->SetPassword(before.GetPassword());
      account->SetPasswordEncryption((int) before.GetPasswordEncryption());
      account->SetPasswordChanged(before.GetPasswordChanged());

      String saveError;
      if (!PersistentAccount::SaveObject(account, saveError, true, PersistenceModeNormal))
      {
         left += (left.IsEmpty() ? String() : String(_T(", "))) + _T("the account's names, switch, external id and password");
         whole = false;
      }
      else if (keyBefore && !MailboxKeys::RestoreWraps(account->GetID(), *keyBefore))
      {
         left += (left.IsEmpty() ? String() : String(_T(", "))) + _T("the mailbox key's wrap under the password put back");
         whole = false;
      }

      return whole;
   }

   // The answer to a change that failed after its first write: the refusal as
   // it was when everything was put back and nothing was renamed, since then
   // nothing changed; otherwise what did happen, in the sentence, and 500. Both
   // diagnostics name the address, and neither fires on a change that was
   // refused before it wrote anything.
   bool ScimFailedPartWay(const ScimContext &scim, const String &oldAddress, const String &address, bool renamed, bool putBack,
                          const String &left, ScimRefusal &refusal)
   {
      const String reason = refusal.detail;

      if (!putBack)
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7223, "RestApiServer::HandleScim_",
            Formatter::Format(_T("A change to {0} over SCIM failed part way ({1}), and what it had written could not all be put back: {2}. Look at the account and its aliases."),
               address, reason, left));
      }

      if (!renamed)
      {
         if (putBack)
            return false;

         return ScimRefuse(refusal, 500, "", reason + _T(" - and the change could not all be put back: ") + left);
      }

      ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7221, "RestApiServer::HandleScim_",
         Formatter::Format(_T("{0} was renamed {1} over SCIM, and the rest of the same change was not made: {2}"), oldAddress, address, reason));

      LOG_APPLICATION(_T("REST API: SCIM ") + scim.identity + _T(" renamed ") + oldAddress + _T(" to ") + address + _T(", and the rest of the change failed: ") + reason);

      return ScimRefuse(refusal, 500, "",
         _T("the account was renamed, and is at ") + address + _T(" now - that is its id - but the rest of the change was not made (") + reason +
         (putBack ? String(_T(")")) : String(_T("), and could not all be put back: ")) + left));
   }

   // Writes a change to an existing account, all of it or none of it.
   //
   // Everything is judged before anything is written: the change itself; the
   // rename's target, and whether the mailbox is in use; every new alias
   // against what its save will ask; each domain's alias limit at the fullest
   // point the writes reach; the account's own save (PreSaveLimitationsCheck);
   // and the password - the policy, the reuse history and the breach check. A
   // refusal there writes nothing at all.
   //
   // Then the writes, in the order that leaves the least to put back. The
   // rename first: MailboxMove::Move judges again and either completes or
   // leaves the account as it was (MailboxMove.h); a rename onto one of the
   // account's own aliases removes that alias just before, and makes it again
   // if the move is refused. Then the account's own columns, then the aliases
   // removed, then the aliases made. A failure after that - the database
   // refusing, or another request taking an address between the judging and
   // the writing - puts back what this change wrote (ScimPutBack). A rename
   // that happened is not undone, since that would be a second move of the
   // mailbox; the answer says where the account now is. The old password's
   // hash goes into the reuse history last, once the whole change is written.
   //
   // A suspension ends the account's sessions and drops its live connections
   // as soon as its row says so, and so does a new password - on every write
   // that leaves the account suspended, not only the one that suspends it, so
   // that a retry ends whatever an earlier attempt's failure left open.
   bool ScimCommitUser(const ScimContext &scim, ScimUserRecord &user, ScimUserChange &change, ScimRefusal &refusal)
   {
      ScimFinishChange(change);
      if (!ScimValidateChange(change, refusal))
         return false;

      std::shared_ptr<Account> account = user.account;
      const std::shared_ptr<const Account> before(new Account(*account));
      const String oldAddress = account->GetAddress();

      // Without regard to case, as userName is compared: a client that writes the
      // address back in another capitalisation - which would otherwise be a
      // mailbox move, refused while the mailbox is in use - is not renaming it,
      // and the account keeps the spelling it has.
      const bool renaming = change.address.CompareNoCase(oldAddress) != 0;
      if (!renaming)
         change.address = oldAddress;

      // A rename onto one of the account's own aliases - an identity provider
      // making a person's alias their address, which it cannot send as two
      // steps - takes the alias's place: the alias gives way just before the
      // move, which refuses an address an alias has.
      std::shared_ptr<Alias> givesWay;

      if (renaming)
      {
         std::shared_ptr<Domain> newDomain;
         if (!ScimProvisionable(scim, change.address, newDomain))
            return ScimRefuse(refusal, 400, "invalidValue", ScimOutsideSentence(_T("userName")));

         givesWay = ScimAliasAmong(user.aliases, change.address);

         if (!givesWay)
         {
            if (ScimFindAccountId(change.address) != 0)
               return ScimRefuse(refusal, 409, "uniqueness", _T("An account already exists at ") + change.address + _T("."));

            if (ScimAliasNamed(change.address))
               return ScimRefuse(refusal, 409, "uniqueness", _T("An alias already exists at ") + change.address + _T(", and it is not one of this account's."));

            if (ScimListAt(scim, change.address))
               return ScimRefuse(refusal, 409, "uniqueness", _T("A distribution list already exists at ") + change.address + _T("."));
         }

         const String inUse = MailboxMove::InUseReason(account);
         if (!inUse.IsEmpty())
            return ScimRefuse(refusal, 409, "", inUse);
      }

      std::vector<String> current;
      for (const std::shared_ptr<Alias> &alias : user.aliases)
         current.push_back(alias->GetName());

      // The old address a rename keeps is not the request's to create, and a
      // request that renames does not remove it: it was not among the aliases
      // the identity provider could have seen. A later PUT or PATCH that leaves
      // it out of emails does remove it (docs/Scim.md).
      std::vector<String> create;
      for (const String &wanted : change.aliases)
      {
         if (ScimContainsNoCase(current, wanted))
            continue;
         if (renaming && wanted.CompareNoCase(oldAddress) == 0)
            continue;
         create.push_back(wanted);
      }

      std::vector<std::shared_ptr<Alias> > remove;
      for (const std::shared_ptr<Alias> &alias : user.aliases)
      {
         if (givesWay && alias->GetID() == givesWay->GetID())
            continue;

         if (!ScimContainsNoCase(change.aliases, alias->GetName()))
            remove.push_back(alias);
      }

      for (const String &name : create)
      {
         if (!ScimCheckNewAlias(scim, name, refusal))
            return false;
      }

      std::vector<ScimDomainAliases> counted;
      if (givesWay)
         ScimAliasesIn(counted, StringParser::ExtractDomain(givesWay->GetName())).gave_way++;
      if (renaming)
         ScimAliasesIn(counted, StringParser::ExtractDomain(oldAddress)).kept++;
      for (const std::shared_ptr<Alias> &alias : remove)
         ScimAliasesIn(counted, StringParser::ExtractDomain(alias->GetName())).removed++;
      for (const String &name : create)
         ScimAliasesIn(counted, StringParser::ExtractDomain(name)).created++;

      if (!ScimJudgeAliasLimits(counted, refusal))
         return false;

      // The account's own save, judged on a copy as it will be: its columns, at
      // the address it is at now - the rename's target is the move's to judge.
      std::shared_ptr<Account> judged(new Account(*account));
      judged->SetPersonFirstName(change.first);
      judged->SetPersonLastName(change.last);
      judged->SetActive(change.active);
      judged->SetExternalId(change.external_id);

      String judgedError;
      if (!PreSaveLimitationsCheck::CheckLimitations(PersistenceModeNormal, judged, judgedError))
         return ScimRefuse(refusal, 400, "invalidValue", judgedError);

      if (change.password_given && !ScimJudgePassword(account, change.password, false, refusal))
         return false;

      // ---- Everything is judged. The writes.

      bool renamed = false;

      if (renaming)
      {
         if (givesWay && !PersistentAlias::DeleteObject(givesWay))
            return ScimRefuse(refusal, 500, "", _T("the alias ") + givesWay->GetName() + _T(" could not be removed to make way for the rename, so nothing was changed"));

         // The explicit operation, keeping the old address as an alias, which
         // is what keeps mail to it arriving: everything keyed on the account
         // follows it, the store and the references are moved, and the audit
         // trail records the move as one entry naming both addresses
         // (MailboxMove.h).
         MailboxMoveOutcome outcome;
         String moveError;

         if (!MailboxMove::Move(account, change.address, true, outcome, moveError))
         {
            if (givesWay && !ScimRestoreAlias(givesWay, oldAddress))
            {
               ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7223, "RestApiServer::HandleScim_",
                  Formatter::Format(_T("A rename of {0} to {1} over SCIM was refused ({2}), and the alias {1}, removed to make way for it, could not be made again. Add it by hand."),
                     oldAddress, change.address, moveError));
            }

            // Whether the mailbox has a key not known just now: for now.
            if (moveError.Find(_T("could not be read just now")) >= 0)
               return ScimRefuse(refusal, 503, "", moveError);

            const bool taken = moveError.Find(_T("already exists")) >= 0;
            const bool conflict = taken || !MailboxMove::InUseReason(account).IsEmpty() ||
                                  moveError.Find(_T("maximum number of accounts")) >= 0 ||
                                  moveError.Find(_T("maximum size for the domain")) >= 0 ||
                                  moveError.Find(_T("legal hold")) >= 0 ||
                                  moveError.Find(_T("encrypted at rest")) >= 0 ||
                                  moveError.Find(_T("has a mailbox key")) >= 0;

            return ScimRefuse(refusal, conflict ? 409 : 400, taken ? "uniqueness" : (conflict ? "" : "invalidValue"), moveError);
         }

         renamed = true;
         LOG_APPLICATION(_T("REST API: SCIM ") + scim.identity + _T(" renamed ") + oldAddress + _T(": ") + MailboxMove::Describe(outcome));
      }

      // The mailbox key's wraps as they are before the password changes, for a
      // put-back to put back with the password (ScimPutBack).
      MailboxKeys::Row keyBefore;
      const bool keyedBefore = change.password_given && MailboxKeys::GetRow(account->GetID(), keyBefore);

      if (change.password_given)
         ScimSetPasswordHash(account, change.password);

      account->SetPersonFirstName(change.first);
      account->SetPersonLastName(change.last);
      account->SetActive(change.active);
      account->SetExternalId(change.external_id);

      std::vector<std::shared_ptr<Alias> > removed;
      std::vector<std::shared_ptr<Alias> > made;

      String saveError;
      if (!PersistentAccount::SaveObject(account, saveError, true, PersistenceModeNormal))
      {
         if (saveError.IsEmpty())
            ScimRefuse(refusal, 500, "", _T("the account could not be saved"));
         else
            ScimRefuse(refusal, 400, "invalidValue", saveError);

         // Nothing of this save reached the row, and nothing after it ran.
         return ScimFailedPartWay(scim, oldAddress, account->GetAddress(), renamed, true, String(), refusal);
      }

      if (!change.active || change.password_given)
      {
         ScimEndSessions(account->GetID());

         if (!change.active)
            LOG_APPLICATION(_T("REST API: SCIM ") + scim.identity + _T(" suspended ") + account->GetAddress() + _T(", and its sessions and connections were ended."));
      }

      bool failed = false;

      for (const std::shared_ptr<Alias> &alias : remove)
      {
         if (!PersistentAlias::DeleteObject(alias))
         {
            ScimRefuse(refusal, 500, "", _T("the alias ") + alias->GetName() + _T(" could not be removed"));
            failed = true;
            break;
         }

         removed.push_back(alias);
      }

      for (size_t index = 0; !failed && index < create.size(); index++)
      {
         std::shared_ptr<Alias> alias;
         if (!ScimCreateAlias(create[index], account->GetAddress(), alias, refusal))
         {
            failed = true;
            break;
         }

         made.push_back(alias);
      }

      if (failed)
      {
         String left;
         const bool putBack = ScimPutBack(account, *before, removed, made, keyedBefore ? &keyBefore : nullptr, left);
         return ScimFailedPartWay(scim, oldAddress, account->GetAddress(), renamed, putBack, left, refusal);
      }

      // The old hash into the reuse history, now that the new one is the
      // account's for good (PasswordHistory::Record reads it from the object).
      if (change.password_given)
         PasswordHistory::Record(before);

      LOG_APPLICATION(_T("REST API: SCIM ") + scim.identity + _T(" changed account ") + account->GetAddress() + _T("."));
      return true;
   }

   // --- the user routes

   HttpResponse ScimListUsers(const ScimContext &scim, const AnsiString &filterText, int start, int count, const ScimProjection &projection)
   {
      ScimFilter filter;
      if (!ScimParseFilter(std::string(filterText.c_str()), filter) || !ScimUserFilterAnswered(filter))
         return ScimInvalidFilter(true);

      std::vector<__int64> ids;

      if (filter.present && filter.attribute == "username" && filter.op == "eq")
      {
         // One address, which is one account at most.
         if (scim.Reaches(StringParser::ExtractDomain(filter.value)))
         {
            const __int64 id = ScimFindAccountId(filter.value);
            if (id != 0)
               ids.push_back(id);
         }
      }
      else if (filter.present && filter.attribute == "externalid")
      {
         std::vector<ScimCandidate> found;
         if (!ScimAccountsByExternalId(filter.value, found))
            return ScimError(500, "", _T("the accounts could not be read"));

         for (const ScimCandidate &candidate : found)
         {
            if (scim.Reaches(StringParser::ExtractDomain(candidate.address)))
               ids.push_back(candidate.id);
         }
      }
      else if (filter.present && (filter.attribute == "emails.value" || filter.attribute == "emails"))
      {
         // The account's own address, or an alias of one - an alias the key
         // reaches, of an account the key reaches.
         __int64 id = scim.Reaches(StringParser::ExtractDomain(filter.value)) ? ScimFindAccountId(filter.value) : 0;

         if (id == 0)
         {
            std::shared_ptr<Alias> alias = ScimAliasNamed(filter.value);
            if (alias && scim.Reaches(StringParser::ExtractDomain(alias->GetName())) &&
                scim.Reaches(StringParser::ExtractDomain(alias->GetValue())))
               id = ScimFindAccountId(alias->GetValue());
         }

         if (id != 0)
            ids.push_back(id);
      }
      else
      {
         std::vector<ScimCandidate> all;
         if (!ScimAllAccounts(all))
            return ScimError(500, "", _T("the accounts could not be read"));

         String wanted = filter.value;
         wanted.MakeLower();

         for (const ScimCandidate &candidate : all)
         {
            if (!scim.Reaches(StringParser::ExtractDomain(candidate.address)))
               continue;

            if (filter.present && filter.attribute == "active" && candidate.active != filter.flag)
               continue;

            if (filter.present && filter.attribute == "username")
            {
               String address = candidate.address;
               address.MakeLower();

               if (filter.op == "co" && address.Find(wanted) < 0)
                  continue;

               if (filter.op == "sw" && address.Left(wanted.GetLength()) != wanted)
                  continue;
            }

            ids.push_back(candidate.id);
         }
      }

      AnsiString resources;
      int emitted = 0;

      // A page of more than one account reads the aliases once, rather than
      // once for each account: the column they are found by has no index, and
      // a page is up to 200 accounts. The order is ScimReadAliases', so that a
      // version answered here is the version the account's own GET answers.
      const size_t first = (size_t) (start - 1);
      const bool wholePage = count > 1 && first + 1 < ids.size();
      std::vector<std::shared_ptr<Alias> > everyAlias;
      if (wholePage)
         ScimReadEveryAlias(scim, everyAlias);

      for (size_t index = first; index < ids.size() && emitted < count; index++)
      {
         ScimUserRecord user;
         user.account = std::shared_ptr<Account>(new Account());

         // An account removed between the two reads is left out of the page,
         // not answered half.
         if (!PersistentAccount::ReadObject(user.account, ids[index]) || user.account->GetID() == 0)
            continue;

         if (wholePage)
            user.aliases = ScimAliasesOf(everyAlias, user.account->GetAddress());
         else
            ScimReadAliases(scim, user.account->GetAddress(), user.aliases);

         const ScimResource resource = ScimUserResource(user);
         const AnsiString location = ScimLocation(scim, "Users", user.account->GetAddress());

         if (emitted > 0)
            resources += ",";
         resources += ScimDocument(resource, &projection, ScimMeta("User", location, ScimVersion(resource)));
         emitted++;
      }

      return ScimRespond(200, ScimListResponse((int) ids.size(), start, emitted, resources), "");
   }

   HttpResponse ScimGetUser(const ScimContext &scim, const String &id, const ScimProjection &projection)
   {
      ScimUserRecord user;
      if (!ScimLoadUser(scim, id, user))
         return ScimNotFound(_T("user"));

      return ScimUserAnswer(scim, 200, user, &projection, false);
   }

   HttpResponse ScimCreateUser(const ScimContext &scim, const AnsiString &requestBody)
   {
      JsonValue body;
      if (!ScimParseBody(requestBody, body))
         return ScimError(400, "invalidSyntax", _T("the body must be a JSON object: a SCIM User"));

      ScimRefusal refusal;
      ScimUserChange change;

      if (!ScimReadUserBody(body, change, refusal))
         return ScimRefused(refusal);

      ScimFinishChange(change);
      if (!ScimValidateChange(change, refusal))
         return ScimRefused(refusal);

      std::shared_ptr<Domain> domain;
      if (!ScimProvisionable(scim, change.address, domain))
         return ScimError(400, "invalidValue", ScimOutsideSentence(_T("userName")));

      if (ScimFindAccountId(change.address) != 0)
         return ScimError(409, "uniqueness", _T("an account at that address exists"));

      if (ScimAliasNamed(change.address))
         return ScimError(409, "uniqueness", _T("that address is an alias"));

      if (ScimListAt(scim, change.address))
         return ScimError(409, "uniqueness", _T("that address is a distribution list's"));

      std::vector<ScimDomainAliases> counted;
      for (const String &alias : change.aliases)
      {
         if (!ScimCheckNewAlias(scim, alias, refusal))
            return ScimRefused(refusal);

         ScimAliasesIn(counted, StringParser::ExtractDomain(alias)).created++;
      }

      if (!ScimJudgeAliasLimits(counted, refusal))
         return ScimRefused(refusal);

      std::shared_ptr<Account> account(new Account());
      account->SetDomainID(domain->GetID());
      account->SetAddress(change.address);
      account->SetActive(change.active);
      account->SetPersonFirstName(change.first);
      account->SetPersonLastName(change.last);
      account->SetExternalId(change.external_id);

      // The largest size the domain allows, which is no limit where it sets
      // none: the domain's limitation check refuses an account with no size in
      // a domain that has a maximum.
      account->SetAccountMaxSize(domain->GetMaxAccountSize());

      if (change.password_given)
      {
         if (!ScimJudgePassword(account, change.password, true, refusal))
            return ScimRefused(refusal);

         ScimSetPasswordHash(account, change.password);
      }
      else
      {
         String unusable;
         if (!ScimUnusablePassword(unusable))
            return ScimError(500, "", _T("the random generator failed, so no account was made"));

         ScimSetPasswordHash(account, unusable);
      }

      // createInbox true, as every other door that makes an account passes it:
      // an account without an INBOX row takes mail at RCPT TO and loses it.
      String saveError;
      if (!PersistentAccount::SaveObject(account, saveError, true, PersistenceModeNormal))
      {
         if (saveError.IsEmpty())
            return ScimError(500, "", _T("the account could not be made"));

         const bool taken = saveError.Find(_T("already exists")) >= 0;
         return ScimError(taken ? 409 : 400, taken ? "uniqueness" : "invalidValue", saveError);
      }

      // SaveObject answers true for a row it has had to delete again because
      // its inbox could not be made; the database is asked.
      ScimUserRecord user;
      user.account = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(user.account, account->GetID()) || user.account->GetID() == 0)
         return ScimError(500, "", _T("the account could not be made: it was not there after it was saved, which is what an inbox that could not be made leaves"));

      std::vector<std::shared_ptr<Alias> > made;

      for (const String &alias : change.aliases)
      {
         std::shared_ptr<Alias> madeAlias;
         if (ScimCreateAlias(alias, user.account->GetAddress(), madeAlias, refusal))
         {
            made.push_back(madeAlias);
            continue;
         }

         // Nothing is left half made: the aliases this request made, and the
         // account - and nothing else. An alias that already pointed at the
         // address before the request, one an administrator made ready for the
         // person or one a removed account left, is not the request's to remove.
         bool undone = true;
         for (const std::shared_ptr<Alias> &undo : made)
            undone = PersistentAlias::DeleteObject(undo) && undone;

         {
            // This request's own account, made a moment ago: a hold on its
            // domain does not keep an account nothing was ever in
            // (DiscoveryHolds::UndoingCreation).
            DiscoveryHolds::UndoingCreation undoing;
            undone = PersistentAccount::DeleteObject(user.account) && undone;
         }

         if (!undone)
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7220, "RestApiServer::HandleScim_",
               Formatter::Format(_T("The account {0} was made over SCIM and one of its aliases was refused, and the account and its aliases could not all be removed again. Remove what is left of {0} by hand."),
                  user.account->GetAddress()));
         }

         return ScimRefused(refusal);
      }

      ScimReadAliases(scim, user.account->GetAddress(), user.aliases);

      LOG_APPLICATION(_T("REST API: SCIM ") + scim.identity + _T(" made account ") + user.account->GetAddress() + _T("."));

      return ScimUserAnswer(scim, 201, user, nullptr, true);
   }

   HttpResponse ScimReplaceUser(const ScimContext &scim, const String &id, const AnsiString &requestBody, const AnsiString &ifMatch)
   {
      ScimUserRecord user;
      if (!ScimLoadUser(scim, id, user))
         return ScimNotFound(_T("user"));

      ScimRefusal refusal;
      if (!ScimMayChange(scim, user.account, refusal))
         return ScimRefused(refusal);

      if (!ScimVersionMatches(ifMatch, ScimVersion(ScimUserResource(user))))
         return ScimVersionRefused();

      JsonValue body;
      if (!ScimParseBody(requestBody, body))
         return ScimError(400, "invalidSyntax", _T("the body must be a JSON object: a SCIM User"));

      ScimUserChange change = ScimCurrentState(user);
      if (!ScimReadUserBody(body, change, refusal))
         return ScimRefused(refusal);

      if (!ScimCommitUser(scim, user, change, refusal))
         return ScimRefused(refusal);

      const __int64 accountId = user.account->GetID();
      ScimUserRecord after;
      after.account = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(after.account, accountId))
         return ScimError(500, "", _T("the account could not be read back"));

      ScimReadAliases(scim, after.account->GetAddress(), after.aliases);
      return ScimUserAnswer(scim, 200, after, nullptr, false);
   }

   // The operations of a PatchOp body, each an object.
   bool ScimReadOperations(const JsonValue &body, std::vector<const JsonValue *> &operations, ScimRefusal &refusal)
   {
      const JsonValue *list = ScimAttr(body, "Operations");
      if (!list || !list->IsArray() || list->Size() == 0)
         return ScimRefuse(refusal, 400, "invalidSyntax", _T("a PATCH body is a PatchOp, with at least one operation in Operations"));

      for (const JsonValue &operation : list->Items())
      {
         if (!operation.IsObject())
            return ScimRefuse(refusal, 400, "invalidSyntax", _T("each operation is an object of op, path and value"));
         operations.push_back(&operation);
      }

      return true;
   }

   bool ScimReadOperation(const JsonValue &operation, std::string &op, std::string &path, const JsonValue *&value, ScimRefusal &refusal)
   {
      const JsonValue *opMember = ScimAttr(operation, "op");
      if (!opMember || !opMember->IsString())
         return ScimRefuse(refusal, 400, "invalidSyntax", _T("each operation names op: add, replace or remove"));

      op = ScimLower(ScimTrim(opMember->AsString()));
      if (op != "add" && op != "replace" && op != "remove")
         return ScimRefuse(refusal, 400, "invalidSyntax", _T("each operation names op: add, replace or remove"));

      path.clear();
      const JsonValue *pathMember = ScimAttr(operation, "path");
      if (pathMember && !pathMember->IsNull())
      {
         if (!pathMember->IsString())
            return ScimRefuse(refusal, 400, "invalidPath", _T("path is a string"));
         path = pathMember->AsString();
      }

      value = ScimAttr(operation, "value");
      if (op != "remove" && !value)
         return ScimRefuse(refusal, 400, "invalidValue", _T("add and replace carry a value"));

      return true;
   }

   HttpResponse ScimPatchUser(const ScimContext &scim, const String &id, const AnsiString &requestBody, const AnsiString &ifMatch)
   {
      ScimUserRecord user;
      if (!ScimLoadUser(scim, id, user))
         return ScimNotFound(_T("user"));

      ScimRefusal refusal;
      if (!ScimMayChange(scim, user.account, refusal))
         return ScimRefused(refusal);

      if (!ScimVersionMatches(ifMatch, ScimVersion(ScimUserResource(user))))
         return ScimVersionRefused();

      JsonValue body;
      if (!ScimParseBody(requestBody, body))
         return ScimError(400, "invalidSyntax", _T("the body must be a JSON object: a SCIM PatchOp"));

      std::vector<const JsonValue *> operations;
      if (!ScimReadOperations(body, operations, refusal))
         return ScimRefused(refusal);

      // Every operation applied, in order, to the account as it will be; then
      // the whole written at once, or nothing when any one is refused (RFC 7644
      // section 3.5.2: a PATCH is atomic).
      ScimUserChange change = ScimCurrentState(user);

      for (const JsonValue *operation : operations)
      {
         std::string op;
         std::string path;
         const JsonValue *value = nullptr;

         if (!ScimReadOperation(*operation, op, path, value, refusal))
            return ScimRefused(refusal);

         if (!ScimApplyUserOperation(op, path, value, change, refusal))
            return ScimRefused(refusal);
      }

      if (!ScimCommitUser(scim, user, change, refusal))
         return ScimRefused(refusal);

      const __int64 accountId = user.account->GetID();
      ScimUserRecord after;
      after.account = std::shared_ptr<Account>(new Account());
      if (!PersistentAccount::ReadObject(after.account, accountId))
         return ScimError(500, "", _T("the account could not be read back"));

      ScimReadAliases(scim, after.account->GetAddress(), after.aliases);
      return ScimUserAnswer(scim, 200, after, nullptr, false);
   }

   HttpResponse ScimDeleteUser(const ScimContext &scim, const String &id, const AnsiString &ifMatch)
   {
      ScimUserRecord user;
      if (!ScimLoadUser(scim, id, user))
         return ScimNotFound(_T("user"));

      ScimRefusal refusal;
      if (!ScimMayChange(scim, user.account, refusal))
         return ScimRefused(refusal);

      if (!ScimVersionMatches(ifMatch, ScimVersion(ScimUserResource(user))))
         return ScimVersionRefused();

      const String address = user.account->GetAddress();

      // The account first, so that a delete the database refuses leaves
      // everything as it was, its aliases and its lists included; then what
      // names its address. PersistentAccount's delete leaves hm_aliases and
      // the lists' rows alone, since they name an address and no account: the
      // aliases emails answered for it go - an alias left behind would take
      // mail for somebody who is gone - and so does the address from every
      // distribution list (ScimRemoveFromLists). What could not be removed
      // after the account was is named in the answer and in HM7224, since the
      // account itself is gone and a retry finds nothing to remove.
      //
      // Not while a legal hold covers the account (docs/Discovery.md): the
      // account and its mail stay, and the identity provider hears 409 until
      // the hold is released. But the person it removed has left, and a held
      // mailbox is not one its former owner should go on reading or changing
      // while the case lasts: the account is suspended, as a SCIM active=false
      // suspends it, and every session and connection of it is ended, before
      // the answer.
      if (DiscoveryHolds::IsAccountHeld(user.account->GetID()))
      {
         String suspended = _T(" The account has been suspended and its sessions ended instead.");

         if (user.account->GetActive())
         {
            user.account->SetActive(false);

            String saveError;
            if (!PersistentAccount::SaveObject(user.account, saveError, true, PersistenceModeNormal))
               suspended = _T(" The account could not be suspended") + (saveError.IsEmpty() ? String() : String(_T(": ")) + saveError) + _T("; its sessions were ended.");
         }

         ScimEndSessions(user.account->GetID());

         LOG_APPLICATION(_T("REST API: SCIM ") + scim.identity + _T(" asked to remove ") + address + _T(", which is under a legal hold, so it was kept.") + suspended);

         return ScimError(409, "", DiscoveryHolds::DeleteAccountRefusal(address) + suspended);
      }

      ScimEndSessions(user.account->GetID());

      if (!PersistentAccount::DeleteObject(user.account))
         return ScimError(500, "", _T("the account could not be removed: its sessions were ended, and nothing else was changed"));

      String left;
      for (const std::shared_ptr<Alias> &alias : user.aliases)
      {
         if (!PersistentAlias::DeleteObject(alias))
            left += (left.IsEmpty() ? String() : String(_T(", "))) + _T("the alias ") + alias->GetName();
      }

      int memberships = 0;
      String lists;
      if (!ScimRemoveFromLists(address, memberships, lists))
         left += (left.IsEmpty() ? String() : String(_T(", "))) + _T("its rows in ") + lists;

      if (!left.IsEmpty())
      {
         ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7224, "RestApiServer::HandleScim_",
            Formatter::Format(_T("The account {0} was removed over SCIM, and not everything naming its address could be removed with it: {1}. Remove it by hand."),
               address, left));

         return ScimError(500, "", _T("the account was removed, and not everything naming its address could be: ") + left);
      }

      LOG_APPLICATION(Formatter::Format(_T("REST API: SCIM {0} removed account {1}, its aliases and its {2} row(s) in distribution lists."),
         scim.identity, address, memberships));

      return ScimRespond(204, AnsiString(), "");
   }

   // ------------------------------------------------------------------ groups

   struct ScimGroupRecord
   {
      std::shared_ptr<DistributionList> list;
      std::vector<String> members;
   };

   bool ScimLoadGroup(const ScimContext &scim, const String &id, ScimGroupRecord &group)
   {
      group.list = ScimListAt(scim, id);
      if (!group.list)
         return false;

      group.members = ScimMemberAddresses(*group.list);
      return true;
   }

   ScimResource ScimGroupResource(const ScimGroupRecord &group)
   {
      const String address = group.list->GetAddress();

      ScimResource resource;
      resource.push_back(std::make_pair("schemas", AnsiString("[\"") + ScimGroupSchema + "\"]"));
      resource.push_back(std::make_pair("id", ScimQuote(address)));
      resource.push_back(std::make_pair("displayName", ScimQuote(address)));

      AnsiString members = "[";
      for (size_t index = 0; index < group.members.size(); index++)
      {
         if (index > 0)
            members += ",";
         members += "{\"value\":" + ScimQuote(group.members[index]) + "}";
      }
      members += "]";

      resource.push_back(std::make_pair("members", members));
      return resource;
   }

   HttpResponse ScimGroupAnswer(const ScimContext &scim, int status, const ScimGroupRecord &group, const ScimProjection *projection, bool created)
   {
      const ScimResource resource = ScimGroupResource(group);
      const AnsiString version = ScimVersion(resource);
      const AnsiString location = ScimLocation(scim, "Groups", group.list->GetAddress());

      AnsiString headers = "ETag: " + version + "\r\n";
      if (created)
         headers += "Location: " + location + "\r\n";

      return ScimRespond(status, ScimDocument(resource, projection, ScimMeta("Group", location, version)), headers);
   }

   struct ScimGroupChange
   {
      String address;                  // displayName, as it will be
      std::vector<String> members;     // as they will be
      std::vector<String> named;       // every member address the request itself sent
   };

   bool ScimReadMemberValue(const JsonValue &entry, String &address)
   {
      if (!entry.IsObject())
         return false;

      const JsonValue *value = ScimAttr(entry, "value");
      if (!value || !ScimReadText(*value, address))
         return false;

      address.Trim();
      return !address.IsEmpty();
   }

   bool ScimReadMembers(const JsonValue &value, std::vector<String> &addresses, ScimRefusal &refusal)
   {
      const String sentence = _T("members is an array of {value}, each value a member's address");

      if (value.IsObject())
      {
         String address;
         if (!ScimReadMemberValue(value, address))
            return ScimRefuse(refusal, 400, "invalidValue", sentence);

         addresses.push_back(address);
         return true;
      }

      if (!value.IsArray())
         return ScimRefuse(refusal, 400, "invalidValue", sentence);

      for (const JsonValue &entry : value.Items())
      {
         String address;
         if (!ScimReadMemberValue(entry, address))
            return ScimRefuse(refusal, 400, "invalidValue", sentence);

         addresses.push_back(address);
      }

      return true;
   }

   bool ScimReadGroupBody(const JsonValue &body, ScimGroupChange &change, ScimRefusal &refusal)
   {
      const JsonValue *displayName = ScimAttr(body, "displayName");
      if (!displayName || !ScimReadText(*displayName, change.address))
         return ScimRefuse(refusal, 400, "invalidValue", _T("displayName is required: the distribution list's address"));

      change.address.Trim();

      for (const std::pair<std::string, JsonValue> &member : body.Members())
      {
         const std::string &key = member.first;

         if (ScimSameName(key, "displayName"))
            continue;

         if (ScimSameName(key, "members"))
         {
            change.members.clear();
            if (!member.second.IsNull() && !ScimReadMembers(member.second, change.members, refusal))
               return false;

            change.named.insert(change.named.end(), change.members.begin(), change.members.end());
            continue;
         }

         // What a client may send back as it read it; a group's externalId,
         // which hm_distributionlists has no column for and is not kept; and an
         // extension's attributes.
         if (ScimSameName(key, "schemas") || ScimSameName(key, "id") || ScimSameName(key, "meta") ||
             ScimSameName(key, "externalId") || ScimStartsWith(key, "urn:"))
            continue;

         return ScimRefuse(refusal, 400, "invalidValue", ScimWide(key.substr(0, 64)) + _T(" is not an attribute of the SCIM Group schema (docs/Scim.md)"));
      }

      return true;
   }

   bool ScimApplyGroupOperation(const std::string &op, const std::string &path, const JsonValue *value, ScimGroupChange &change, ScimRefusal &refusal)
   {
      std::string lowered = ScimLower(ScimTrim(path));

      if (lowered.empty())
      {
         if (op == "remove")
            return ScimRefuse(refusal, 400, "noTarget", _T("remove needs a path"));

         if (!value || !value->IsObject())
            return ScimRefuse(refusal, 400, "invalidValue", _T("an operation with no path carries an object of attributes and their values"));

         for (const std::pair<std::string, JsonValue> &member : value->Members())
         {
            if (!ScimApplyGroupOperation(op, member.first, &member.second, change, refusal))
               return false;
         }

         return true;
      }

      if (ScimStartsWith(lowered, ScimGroupPathPrefix))
         lowered = lowered.substr(strlen(ScimGroupPathPrefix));
      else if (ScimStartsWith(lowered, "urn:"))
         return true;

      std::string head;
      std::string sub;
      bool filtered = false;
      ScimFilter filter;

      const String pathNamesNothing = _T("the path ") + ScimWide(ScimTrim(path).substr(0, 96)) + _T(" names no attribute of the SCIM Group schema this server reads (docs/Scim.md)");

      if (!ScimSplitPath(lowered, head, filtered, filter, sub))
         return ScimRefuse(refusal, 400, "invalidPath", pathNamesNothing);

      if (head == "externalid" || head == "id" || head == "meta" || head == "schemas")
         return true;

      if (head == "displayname" && !filtered && sub.empty())
      {
         String text;
         if (op == "remove" || !value || !ScimReadText(*value, text))
            return ScimRefuse(refusal, 400, "invalidValue", _T("displayName is the distribution list's address, and is replaced, never removed"));

         text.Trim();
         change.address = text;
         return true;
      }

      if (head != "members" || !sub.empty())
         return ScimRefuse(refusal, 400, "invalidPath", pathNamesNothing);

      if (filtered)
      {
         // members[value eq "..."]: one member, for remove.
         if (op != "remove" || filter.attribute != "value" || filter.op != "eq" || filter.boolean)
            return ScimRefuse(refusal, 400, "invalidPath", _T("members[value eq \"...\"] is for remove; a member is added to members with add"));

         if (!ScimContainsNoCase(change.members, filter.value))
            return ScimRefuse(refusal, 400, "noTarget", _T("the list has no member ") + filter.value);

         ScimRemoveNoCase(change.members, filter.value);
         change.named.push_back(filter.value);
         return true;
      }

      if (op == "remove")
      {
         // Without a value, every member; with one, the members it names -
         // Entra's form - and an address that is no member is nothing to do.
         if (!value || value->IsNull())
         {
            change.members.clear();
            return true;
         }

         std::vector<String> gone;
         if (!ScimReadMembers(*value, gone, refusal))
            return false;

         for (const String &address : gone)
            ScimRemoveNoCase(change.members, address);

         change.named.insert(change.named.end(), gone.begin(), gone.end());
         return true;
      }

      if (!value || value->IsNull())
         return ScimRefuse(refusal, 400, "invalidValue", _T("add and replace of members carry the members"));

      std::vector<String> given;
      if (!ScimReadMembers(*value, given, refusal))
         return false;

      change.named.insert(change.named.end(), given.begin(), given.end());

      if (op == "replace")
         change.members.clear();

      for (const String &address : given)
      {
         if (!ScimContainsNoCase(change.members, address))
            change.members.push_back(address);
      }

      return true;
   }

   // The members a change adds and removes, judged before anything is
   // written. Every address it adds is an e-mail address; not the list's own
   // address, nor another distribution list's - a list inside a list, or inside
   // itself, is expanded again and again to the recursion limit and reaches
   // nobody the identity provider meant; and, for a key confined to named
   // domains, at one of them, since a list is a way to send to its members and a
   // key for one tenant does not decide whose mailboxes elsewhere a message
   // reaches. A confined key is judged on the members it names and on no others:
   // a member outside its domains that an administrator gave the list is neither
   // its to add nor its to remove, so a PUT or a replace that leaves one out
   // leaves it where it is, and an operation that names one is refused, naming
   // the address the request sent. existing is the list's members as they are,
   // empty for a POST; members comes back trimmed, without doubles.
   bool ScimJudgeMembers(const ScimContext &scim, const std::vector<String> &existing, const String &listAddress, const String &oldAddress,
                         const std::vector<String> &named, std::vector<String> &members, ScimRefusal &refusal)
   {
      std::vector<String> unique;
      for (const String &member : members)
      {
         String trimmed = member;
         trimmed.Trim();

         if (!trimmed.IsEmpty() && !ScimContainsNoCase(unique, trimmed))
            unique.push_back(trimmed);
      }

      for (const String &member : existing)
      {
         if (ScimContainsNoCase(unique, member) || scim.Reaches(StringParser::ExtractDomain(member)))
            continue;

         if (ScimContainsNoCase(named, member))
            return ScimRefuse(refusal, 400, "invalidValue", _T("a member this api key removes must be at a domain it provisions, and ") + member + _T(" is not"));

         unique.push_back(member);
      }

      for (const String &member : unique)
      {
         if (ScimContainsNoCase(existing, member))
            continue;

         if (!StringParser::IsValidEmailAddress(member))
            return ScimRefuse(refusal, 400, "invalidValue", _T("every member is an e-mail address, and ") + member + _T(" is not"));

         if (!scim.Reaches(StringParser::ExtractDomain(member)))
            return ScimRefuse(refusal, 400, "invalidValue", _T("a member this api key adds must be at a domain it provisions, and ") + member + _T(" is not"));

         if (member.CompareNoCase(listAddress) == 0 || (!oldAddress.IsEmpty() && member.CompareNoCase(oldAddress) == 0))
            return ScimRefuse(refusal, 400, "invalidValue", _T("a distribution list is not a member of itself, and ") + member + _T(" is this list's address"));

         std::shared_ptr<DistributionList> nested(new DistributionList());
         if (PersistentDistributionList::ReadObject(nested, member) && nested->GetID() > 0)
            return ScimRefuse(refusal, 400, "invalidValue", member + _T(" is a distribution list, and a list is not taken as a member of another here: name its members"));
      }

      members.swap(unique);
      return true;
   }

   // Writes a change to an existing list: its address, then its members.
   bool ScimCommitGroup(const ScimContext &scim, ScimGroupRecord &group, ScimGroupChange &change, ScimRefusal &refusal)
   {
      const String oldAddress = group.list->GetAddress();

      if (!ScimJudgeMembers(scim, group.members, change.address, oldAddress, change.named, change.members, refusal))
         return false;

      if (change.address != oldAddress)
      {
         // A list's address changes here only inside its domain: moving one
         // to another domain would move who may post to it and whose policy it
         // answers to, which is an administrator's decision.
         if (!StringParser::IsValidEmailAddress(change.address) ||
             StringParser::ExtractDomain(change.address).CompareNoCase(StringParser::ExtractDomain(oldAddress)) != 0)
            return ScimRefuse(refusal, 400, "invalidValue", _T("displayName is the distribution list's address, and it may change here only inside the list's own domain"));

         if (change.address.CompareNoCase(oldAddress) != 0)
         {
            if (ScimListAt(scim, change.address))
               return ScimRefuse(refusal, 409, "uniqueness", _T("a distribution list at that address exists"));

            if (ScimFindAccountId(change.address) != 0)
               return ScimRefuse(refusal, 409, "uniqueness", _T("that address is an account's"));

            if (ScimAliasNamed(change.address))
               return ScimRefuse(refusal, 409, "uniqueness", _T("that address is an alias"));
         }

         group.list->SetAddress(change.address);

         String saveError;
         if (!PersistentDistributionList::SaveObject(group.list, saveError, PersistenceModeNormal))
         {
            if (saveError.IsEmpty())
               return ScimRefuse(refusal, 500, "", _T("the distribution list could not be saved"));

            const bool taken = saveError.Find(_T("already exists")) >= 0;
            return ScimRefuse(refusal, taken ? 409 : 400, taken ? "uniqueness" : "invalidValue", saveError);
         }
      }

      const __int64 listId = group.list->GetID();

      for (const String &member : group.members)
      {
         if (!ScimContainsNoCase(change.members, member) && !ScimRemoveMember(listId, member, refusal))
            return false;
      }

      for (const String &member : change.members)
      {
         if (!ScimContainsNoCase(group.members, member) && !ScimAddMember(listId, member, refusal))
            return false;
      }

      LOG_APPLICATION(_T("REST API: SCIM ") + scim.identity + _T(" changed distribution list ") + change.address + _T("."));
      return true;
   }

   // --- the group routes

   HttpResponse ScimListGroups(const ScimContext &scim, const AnsiString &filterText, int start, int count, const ScimProjection &projection)
   {
      ScimFilter filter;
      if (!ScimParseFilter(std::string(filterText.c_str()), filter) || !ScimGroupFilterAnswered(filter))
         return ScimInvalidFilter(false);

      SQLCommand command("select distributionlistid, distributionlistaddress from hm_distributionlists order by distributionlistid");

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return ScimError(500, "", _T("the distribution lists could not be read"));

      std::vector<__int64> ids;
      while (!recordset->IsEOF())
      {
         const String address = recordset->GetStringValue("distributionlistaddress");

         if (scim.Reaches(StringParser::ExtractDomain(address)) &&
             (!filter.present || address.CompareNoCase(filter.value) == 0))
            ids.push_back(recordset->GetLongValue("distributionlistid"));

         recordset->MoveNext();
      }

      AnsiString resources;
      int emitted = 0;

      for (size_t index = (size_t) (start - 1); index < ids.size() && emitted < count; index++)
      {
         ScimGroupRecord group;
         group.list = std::shared_ptr<DistributionList>(new DistributionList());
         if (!PersistentDistributionList::ReadObject(group.list, ids[index]) || group.list->GetID() == 0)
            continue;

         group.members = ScimMemberAddresses(*group.list);

         const ScimResource resource = ScimGroupResource(group);
         const AnsiString location = ScimLocation(scim, "Groups", group.list->GetAddress());

         if (emitted > 0)
            resources += ",";
         resources += ScimDocument(resource, &projection, ScimMeta("Group", location, ScimVersion(resource)));
         emitted++;
      }

      return ScimRespond(200, ScimListResponse((int) ids.size(), start, emitted, resources), "");
   }

   HttpResponse ScimGetGroup(const ScimContext &scim, const String &id, const ScimProjection &projection)
   {
      ScimGroupRecord group;
      if (!ScimLoadGroup(scim, id, group))
         return ScimNotFound(_T("group"));

      return ScimGroupAnswer(scim, 200, group, &projection, false);
   }

   HttpResponse ScimCreateGroup(const ScimContext &scim, const AnsiString &requestBody)
   {
      JsonValue body;
      if (!ScimParseBody(requestBody, body))
         return ScimError(400, "invalidSyntax", _T("the body must be a JSON object: a SCIM Group"));

      ScimRefusal refusal;
      ScimGroupChange change;
      if (!ScimReadGroupBody(body, change, refusal))
         return ScimRefused(refusal);

      std::shared_ptr<Domain> domain;
      if (!ScimProvisionable(scim, change.address, domain))
         return ScimError(400, "invalidValue", ScimOutsideSentence(_T("displayName")));

      if (ScimListAt(scim, change.address))
         return ScimError(409, "uniqueness", _T("a distribution list at that address exists"));

      if (ScimFindAccountId(change.address) != 0)
         return ScimError(409, "uniqueness", _T("that address is an account's"));

      if (ScimAliasNamed(change.address))
         return ScimError(409, "uniqueness", _T("that address is an alias"));

      if (!ScimJudgeMembers(scim, std::vector<String>(), change.address, String(), change.named, change.members, refusal))
         return ScimRefused(refusal);

      // The defaults InterfaceDistributionLists::Add and the REST list route
      // leave: active, no authentication required, public. The list manager is
      // off, and the membership query empty.
      std::shared_ptr<DistributionList> list(new DistributionList());
      list->SetDomainID(domain->GetID());
      list->SetAddress(change.address);
      list->SetActive(true);
      list->SetRequireAuth(false);
      list->SetListMode(DistributionList::LMPublic);

      String saveError;
      if (!PersistentDistributionList::SaveObject(list, saveError, PersistenceModeNormal))
      {
         if (saveError.IsEmpty())
            return ScimError(500, "", _T("the distribution list could not be made"));

         const bool taken = saveError.Find(_T("already exists")) >= 0;
         return ScimError(taken ? 409 : 400, taken ? "uniqueness" : "invalidValue", saveError);
      }

      for (const String &member : change.members)
      {
         if (ScimAddMember(list->GetID(), member, refusal))
            continue;

         // Nothing is left half made.
         if (!PersistentDistributionList::DeleteObject(list))
         {
            ErrorManager::Instance()->ReportError(ErrorManager::Medium, 7222, "RestApiServer::HandleScim_",
               Formatter::Format(_T("The distribution list {0} was made over SCIM and one of its members could not be saved, and the list could not be removed again. Remove it by hand."),
                  change.address));
         }

         return ScimRefused(refusal);
      }

      ScimGroupRecord group;
      if (!ScimLoadGroup(scim, change.address, group))
         return ScimError(500, "", _T("the distribution list could not be read back"));

      LOG_APPLICATION(_T("REST API: SCIM ") + scim.identity + _T(" made distribution list ") + change.address + _T("."));

      return ScimGroupAnswer(scim, 201, group, nullptr, true);
   }

   HttpResponse ScimReplaceGroup(const ScimContext &scim, const String &id, const AnsiString &requestBody, const AnsiString &ifMatch)
   {
      ScimGroupRecord group;
      if (!ScimLoadGroup(scim, id, group))
         return ScimNotFound(_T("group"));

      if (!ScimVersionMatches(ifMatch, ScimVersion(ScimGroupResource(group))))
         return ScimVersionRefused();

      JsonValue body;
      if (!ScimParseBody(requestBody, body))
         return ScimError(400, "invalidSyntax", _T("the body must be a JSON object: a SCIM Group"));

      // Members the body leaves out are not asserted and stay as they are; a
      // members array, empty included, is the whole of them.
      ScimRefusal refusal;
      ScimGroupChange change;
      change.members = group.members;
      if (!ScimReadGroupBody(body, change, refusal))
         return ScimRefused(refusal);

      if (!ScimCommitGroup(scim, group, change, refusal))
         return ScimRefused(refusal);

      ScimGroupRecord after;
      if (!ScimLoadGroup(scim, change.address, after))
         return ScimError(500, "", _T("the distribution list could not be read back"));

      return ScimGroupAnswer(scim, 200, after, nullptr, false);
   }

   HttpResponse ScimPatchGroup(const ScimContext &scim, const String &id, const AnsiString &requestBody, const AnsiString &ifMatch)
   {
      ScimGroupRecord group;
      if (!ScimLoadGroup(scim, id, group))
         return ScimNotFound(_T("group"));

      if (!ScimVersionMatches(ifMatch, ScimVersion(ScimGroupResource(group))))
         return ScimVersionRefused();

      JsonValue body;
      if (!ScimParseBody(requestBody, body))
         return ScimError(400, "invalidSyntax", _T("the body must be a JSON object: a SCIM PatchOp"));

      ScimRefusal refusal;
      std::vector<const JsonValue *> operations;
      if (!ScimReadOperations(body, operations, refusal))
         return ScimRefused(refusal);

      ScimGroupChange change;
      change.address = group.list->GetAddress();
      change.members = group.members;

      for (const JsonValue *operation : operations)
      {
         std::string op;
         std::string path;
         const JsonValue *value = nullptr;

         if (!ScimReadOperation(*operation, op, path, value, refusal))
            return ScimRefused(refusal);

         if (!ScimApplyGroupOperation(op, path, value, change, refusal))
            return ScimRefused(refusal);
      }

      if (!ScimCommitGroup(scim, group, change, refusal))
         return ScimRefused(refusal);

      ScimGroupRecord after;
      if (!ScimLoadGroup(scim, change.address, after))
         return ScimError(500, "", _T("the distribution list could not be read back"));

      return ScimGroupAnswer(scim, 200, after, nullptr, false);
   }

   HttpResponse ScimDeleteGroup(const ScimContext &scim, const String &id, const AnsiString &ifMatch)
   {
      ScimGroupRecord group;
      if (!ScimLoadGroup(scim, id, group))
         return ScimNotFound(_T("group"));

      if (!ScimVersionMatches(ifMatch, ScimVersion(ScimGroupResource(group))))
         return ScimVersionRefused();

      const String address = group.list->GetAddress();

      // The list's members and its archive go with it (PersistentDistributionList).
      if (!PersistentDistributionList::DeleteObject(group.list))
         return ScimError(500, "", _T("the distribution list could not be removed"));

      LOG_APPLICATION(_T("REST API: SCIM ") + scim.identity + _T(" removed distribution list ") + address + _T("."));

      return ScimRespond(204, AnsiString(), "");
   }

   // ------------------------------------------------------------------ discovery

   // RFC 7643 section 7, for the attributes this server keeps and no others, so
   // that a client that reads the schema learns what is kept. externalId, id and
   // meta are common attributes and are not listed (RFC 7643 section 3.1).
   const char *const ScimUserAttributes =
      "["
      "{\"name\":\"userName\",\"type\":\"string\",\"multiValued\":false,\"description\":\"The account's address, which is also its id.\",\"required\":true,\"caseExact\":false,\"mutability\":\"readWrite\",\"returned\":\"default\",\"uniqueness\":\"server\"},"
      "{\"name\":\"name\",\"type\":\"complex\",\"multiValued\":false,\"description\":\"The person's first and last names, sixty characters each.\",\"required\":false,\"mutability\":\"readWrite\",\"returned\":\"default\",\"uniqueness\":\"none\",\"subAttributes\":["
      "{\"name\":\"givenName\",\"type\":\"string\",\"multiValued\":false,\"description\":\"The first name.\",\"required\":false,\"caseExact\":false,\"mutability\":\"readWrite\",\"returned\":\"default\",\"uniqueness\":\"none\"},"
      "{\"name\":\"familyName\",\"type\":\"string\",\"multiValued\":false,\"description\":\"The last name.\",\"required\":false,\"caseExact\":false,\"mutability\":\"readWrite\",\"returned\":\"default\",\"uniqueness\":\"none\"},"
      "{\"name\":\"formatted\",\"type\":\"string\",\"multiValued\":false,\"description\":\"The two names, composed.\",\"required\":false,\"caseExact\":false,\"mutability\":\"readOnly\",\"returned\":\"default\",\"uniqueness\":\"none\"}]},"
      "{\"name\":\"displayName\",\"type\":\"string\",\"multiValued\":false,\"description\":\"The two names, composed. Written with no name beside it to an account that has no name, it is split at its last space; to one that has, it is accepted and not kept.\",\"required\":false,\"caseExact\":false,\"mutability\":\"readWrite\",\"returned\":\"default\",\"uniqueness\":\"none\"},"
      "{\"name\":\"active\",\"type\":\"boolean\",\"multiValued\":false,\"description\":\"Whether the account may sign in and receive mail; false suspends it, ends its sessions and drops its live connections.\",\"required\":false,\"mutability\":\"readWrite\",\"returned\":\"default\"},"
      "{\"name\":\"emails\",\"type\":\"complex\",\"multiValued\":true,\"description\":\"The account's address (work, primary) and its aliases (other).\",\"required\":false,\"mutability\":\"readWrite\",\"returned\":\"default\",\"uniqueness\":\"none\",\"subAttributes\":["
      "{\"name\":\"value\",\"type\":\"string\",\"multiValued\":false,\"description\":\"The address.\",\"required\":true,\"caseExact\":false,\"mutability\":\"readWrite\",\"returned\":\"default\",\"uniqueness\":\"server\"},"
      "{\"name\":\"type\",\"type\":\"string\",\"multiValued\":false,\"description\":\"work for the account's own address, other for an alias.\",\"required\":false,\"caseExact\":false,\"canonicalValues\":[\"work\",\"other\"],\"mutability\":\"readWrite\",\"returned\":\"default\",\"uniqueness\":\"none\"},"
      "{\"name\":\"primary\",\"type\":\"boolean\",\"multiValued\":false,\"description\":\"True for the account's own address alone.\",\"required\":false,\"mutability\":\"readWrite\",\"returned\":\"default\"}]},"
      "{\"name\":\"password\",\"type\":\"string\",\"multiValued\":false,\"description\":\"Write-only; the password policy, the reuse history and the breach check judge it.\",\"required\":false,\"caseExact\":true,\"mutability\":\"writeOnly\",\"returned\":\"never\",\"uniqueness\":\"none\"}"
      "]";

   const char *const ScimGroupAttributes =
      "["
      "{\"name\":\"displayName\",\"type\":\"string\",\"multiValued\":false,\"description\":\"The distribution list's address, which is also its id.\",\"required\":true,\"caseExact\":false,\"mutability\":\"readWrite\",\"returned\":\"default\",\"uniqueness\":\"server\"},"
      "{\"name\":\"members\",\"type\":\"complex\",\"multiValued\":true,\"description\":\"The list's confirmed members, by address.\",\"required\":false,\"mutability\":\"readWrite\",\"returned\":\"default\",\"subAttributes\":["
      "{\"name\":\"value\",\"type\":\"string\",\"multiValued\":false,\"description\":\"The member's address: a user's id, or an address elsewhere.\",\"required\":true,\"caseExact\":false,\"mutability\":\"immutable\",\"returned\":\"default\",\"uniqueness\":\"none\"}]}"
      "]";

   AnsiString ScimSchemaDocument(const ScimContext &scim, const char *id, const char *name, const char *description, const char *attributes)
   {
      return AnsiString("{\"schemas\":[\"") + ScimSchemaSchema + "\"],\"id\":\"" + id + "\",\"name\":\"" + name +
             "\",\"description\":\"" + description + "\",\"attributes\":" + attributes +
             ",\"meta\":{\"resourceType\":\"Schema\",\"location\":\"" + ScimAsciiEscape(scim.base + "/Schemas/" + id) + "\"}}";
   }

   AnsiString ScimUserSchemaDocument(const ScimContext &scim)
   {
      return ScimSchemaDocument(scim, ScimUserSchema, "User", "An account of this server.", ScimUserAttributes);
   }

   AnsiString ScimGroupSchemaDocument(const ScimContext &scim)
   {
      return ScimSchemaDocument(scim, ScimGroupSchema, "Group", "A distribution list of this server.", ScimGroupAttributes);
   }

   AnsiString ScimResourceTypeDocument(const ScimContext &scim, const char *name, const char *endpoint, const char *description, const char *schema)
   {
      return AnsiString("{\"schemas\":[\"") + ScimResourceTypeSchema + "\"],\"id\":\"" + name + "\",\"name\":\"" + name +
             "\",\"endpoint\":\"" + endpoint + "\",\"description\":\"" + description + "\",\"schema\":\"" + schema +
             "\",\"meta\":{\"resourceType\":\"ResourceType\",\"location\":\"" + ScimAsciiEscape(scim.base + "/ResourceTypes/" + name) + "\"}}";
   }

   AnsiString ScimUserResourceType(const ScimContext &scim)
   {
      return ScimResourceTypeDocument(scim, "User", "/Users", "An account, with its aliases.", ScimUserSchema);
   }

   AnsiString ScimGroupResourceType(const ScimContext &scim)
   {
      return ScimResourceTypeDocument(scim, "Group", "/Groups", "A distribution list.", ScimGroupSchema);
   }

   HttpResponse ScimServiceProviderConfig(const ScimContext &scim)
   {
      AnsiString filter;
      filter.Format("\"filter\":{\"supported\":true,\"maxResults\":%d},", ScimMaxResults);

      const AnsiString body = AnsiString("{\"schemas\":[\"") + ScimServiceProviderConfigSchema + "\"],"
         "\"patch\":{\"supported\":true},"
         "\"bulk\":{\"supported\":false,\"maxOperations\":0,\"maxPayloadSize\":0}," +
         filter +
         "\"changePassword\":{\"supported\":true},"
         "\"sort\":{\"supported\":false},"
         "\"etag\":{\"supported\":true},"
         "\"authenticationSchemes\":[{\"type\":\"oauthbearertoken\",\"name\":\"OAuth Bearer Token\","
         "\"description\":\"An API key of this server - hmapi_ and 64 hexadecimal digits - presented as Authorization: Bearer, whose role holds the provision permission (docs/Scim.md).\","
         "\"primary\":true}],"
         "\"meta\":{\"resourceType\":\"ServiceProviderConfig\",\"location\":\"" + ScimAsciiEscape(scim.base + "/ServiceProviderConfig") + "\"}}";

      return ScimRespond(200, body, "");
   }

   HttpResponse ScimSchemas(const ScimContext &scim, const String &id)
   {
      if (id.IsEmpty())
         return ScimRespond(200, ScimListResponse(2, 1, 2, ScimUserSchemaDocument(scim) + "," + ScimGroupSchemaDocument(scim)), "");

      if (id.CompareNoCase(String(ScimUserSchema)) == 0)
         return ScimRespond(200, ScimUserSchemaDocument(scim), "");

      if (id.CompareNoCase(String(ScimGroupSchema)) == 0)
         return ScimRespond(200, ScimGroupSchemaDocument(scim), "");

      return ScimNotFound(_T("schema"));
   }

   HttpResponse ScimResourceTypes(const ScimContext &scim, const String &id)
   {
      if (id.IsEmpty())
         return ScimRespond(200, ScimListResponse(2, 1, 2, ScimUserResourceType(scim) + "," + ScimGroupResourceType(scim)), "");

      if (id.CompareNoCase(_T("User")) == 0)
         return ScimRespond(200, ScimUserResourceType(scim), "");

      if (id.CompareNoCase(_T("Group")) == 0)
         return ScimRespond(200, ScimGroupResourceType(scim), "");

      return ScimNotFound(_T("resource type"));
   }

   // Everything under /scim/ that is none of the routes above.
   HttpResponse ScimUnknownPath(const AnsiString &method, const AnsiString &rest)
   {
      const std::string path = ScimLower(std::string(rest.c_str()));
      const size_t slash = path.find('/');
      const std::string collection = path.substr(0, slash);
      const std::string id = slash == std::string::npos ? std::string() : path.substr(slash + 1);

      if (collection == "bulk")
         return ScimError(501, "", _T("bulk operations are not supported (ServiceProviderConfig says so); send one request per resource"));

      if (collection == "me")
         return ScimError(501, "", _T("/Me is not supported: this interface is an identity provider's, which names every resource by its id"));

      if ((collection == "users" || collection == "groups") && id == ".search")
         return ScimError(501, "", _T("searching with POST is not supported; GET the collection with filter"));

      if (id.find('/') == std::string::npos &&
          (collection == "users" || collection == "groups" || collection == "serviceproviderconfig" ||
           collection == "schemas" || collection == "resourcetypes"))
         return ScimError(405, "", String(method) + _T(" is not a method this resource takes (docs/Scim.md)"));

      return ScimError(404, "", _T("no SCIM resource is at that path; the base is /scim/v2"));
   }

   // startIndex and count, as RFC 7644 section 3.4.2.4 reads them: startIndex
   // from 1 (a smaller one is 1), count from 0 (a negative one is 0) and at most
   // ScimMaxResults. A value that is not a whole number is refused.
   bool ScimReadInteger(const AnsiString &text, int &value)
   {
      const std::string trimmed = ScimTrim(std::string(text.c_str()));
      if (trimmed.empty() || trimmed.size() > 9)
         return false;

      size_t index = 0;
      bool negative = false;
      if (trimmed[0] == '-')
      {
         negative = true;
         index = 1;
         if (trimmed.size() == 1)
            return false;
      }

      int number = 0;
      for (; index < trimmed.size(); index++)
      {
         if (trimmed[index] < '0' || trimmed[index] > '9')
            return false;
         number = number * 10 + (trimmed[index] - '0');
      }

      value = negative ? -number : number;
      return true;
   }

   bool ScimReadPaging(const AnsiString &startText, const AnsiString &countText, int &start, int &count, ScimRefusal &refusal)
   {
      start = 1;
      count = ScimMaxResults;

      if (!startText.IsEmpty() && !ScimReadInteger(startText, start))
         return ScimRefuse(refusal, 400, "invalidValue", _T("startIndex is a whole number, from 1"));

      if (!countText.IsEmpty() && !ScimReadInteger(countText, count))
         return ScimRefuse(refusal, 400, "invalidValue", _T("count is a whole number, from 0"));

      if (start < 1)
         start = 1;
      if (count < 0)
         count = 0;
      if (count > ScimMaxResults)
         count = ScimMaxResults;

      return true;
   }
}

namespace HM
{
   bool
   RestApiServer::IsScimPath_(const AnsiString &path)
   {
      return path.CompareNoCase("/scim") == 0 || path.StartsWith("/scim/");
   }

   bool
   RestApiServer::IsScimRoute_(RouteKind kind)
   {
      switch (kind)
      {
      case RouteScimServiceProviderConfig:
      case RouteScimSchemas:
      case RouteScimResourceTypes:
      case RouteScimUserList:
      case RouteScimUserGet:
      case RouteScimUserCreate:
      case RouteScimUserReplace:
      case RouteScimUserPatch:
      case RouteScimUserDelete:
      case RouteScimGroupList:
      case RouteScimGroupGet:
      case RouteScimGroupCreate:
      case RouteScimGroupReplace:
      case RouteScimGroupPatch:
      case RouteScimGroupDelete:
      case RouteScimUnknown:
         return true;

      default:
         break;
      }

      return false;
   }

   void
   RestApiServer::ParseScimRoute_(const AnsiString &method, const AnsiString &path, Route &route)
   {
      // Every path here is a SCIM route, the unknown one included, so that the
      // refusals before the handler and the handler's own 404 are all SCIM's.
      // route.name carries the path under /scim/v2/ for the unknown route's
      // answer; route.identifier the resource's id, decoded by ProcessRequest_.
      route.kind = RouteScimUnknown;
      route.name = path;

      const AnsiString prefix = "/scim/v2/";
      if (path.GetLength() <= prefix.GetLength() || !path.StartsWith(prefix))
         return;

      AnsiString rest = path.Mid(prefix.GetLength());
      if (rest.GetLength() > 0 && rest.GetAt(rest.GetLength() - 1) == '/')
         rest = rest.Mid(0, rest.GetLength() - 1);

      route.name = rest;

      const int slash = rest.Find("/");
      const AnsiString collection = slash < 0 ? rest : rest.Mid(0, slash);
      const AnsiString id = slash < 0 ? AnsiString() : rest.Mid(slash + 1);

      if (id.Find("/") >= 0)
         return;

      const bool one = !id.IsEmpty();

      if (collection.CompareNoCase("ServiceProviderConfig") == 0)
      {
         if (method == "GET" && !one)
            route.kind = RouteScimServiceProviderConfig;
         return;
      }

      if (collection.CompareNoCase("Schemas") == 0 || collection.CompareNoCase("ResourceTypes") == 0)
      {
         if (method == "GET")
         {
            route.kind = collection.CompareNoCase("Schemas") == 0 ? RouteScimSchemas : RouteScimResourceTypes;
            route.identifier = id;
         }
         return;
      }

      const bool users = collection.CompareNoCase("Users") == 0;
      const bool groups = collection.CompareNoCase("Groups") == 0;
      if (!users && !groups)
         return;

      if (!one)
      {
         if (method == "GET")
            route.kind = users ? RouteScimUserList : RouteScimGroupList;
         else if (method == "POST")
            route.kind = users ? RouteScimUserCreate : RouteScimGroupCreate;
         return;
      }

      // POST .search is RFC 7644's search by body, which this server does not
      // take: the unknown route answers it 501.
      if (id == ".search")
         return;

      route.identifier = id;

      if (method == "GET")
         route.kind = users ? RouteScimUserGet : RouteScimGroupGet;
      else if (method == "PUT")
         route.kind = users ? RouteScimUserReplace : RouteScimGroupReplace;
      else if (method == "PATCH")
         route.kind = users ? RouteScimUserPatch : RouteScimGroupPatch;
      else if (method == "DELETE")
         route.kind = users ? RouteScimUserDelete : RouteScimGroupDelete;
   }

   HttpResponse
   RestApiServer::BuildScimError_(int statusCode, const AnsiString &scimType, const String &detail, const AnsiString &extraHeaders)
   {
      return ScimRespond(statusCode, ScimErrorDocument(statusCode, scimType, detail), extraHeaders);
   }

   HttpResponse
   RestApiServer::HandleScim_(const Caller &caller, const Route &route, const AnsiString &method, const AnsiString &request)
   {
      // Authorize_ has seen to it that this is an API key whose role holds
      // provision. Everything a handler below decides with is copied here, from
      // the one reading of the key the request was authorised on.
      ScimContext scim;
      scim.domains = caller.domains;
      scim.allowed = &RestApiServer::IsDomainAllowed_;
      scim.grant = caller.grant;
      scim.identity = String(caller.identity);
      scim.base = ScimBaseUrl(GetHeader_(request, "host"), use_tls_);

      const String id = ScimWide(route.identifier);
      const AnsiString ifMatch = GetHeader_(request, "if-match");

      ScimProjection projection;
      ScimReadAttributeList(QueryParameter_(route.query, "attributes"), projection.only);
      ScimReadAttributeList(QueryParameter_(route.query, "excludedAttributes"), projection.excluded);

      if (route.kind == RouteScimUserList || route.kind == RouteScimGroupList)
      {
         int start = 1;
         int count = 0;
         ScimRefusal refusal;
         if (!ScimReadPaging(QueryParameter_(route.query, "startIndex"), QueryParameter_(route.query, "count"), start, count, refusal))
            return ScimRefused(refusal);

         const AnsiString filter = QueryParameter_(route.query, "filter");

         if (route.kind == RouteScimUserList)
            return ScimListUsers(scim, filter, start, count, projection);

         return ScimListGroups(scim, filter, start, count, projection);
      }

      switch (route.kind)
      {
      case RouteScimServiceProviderConfig:
         return ScimServiceProviderConfig(scim);
      case RouteScimSchemas:
         return ScimSchemas(scim, id);
      case RouteScimResourceTypes:
         return ScimResourceTypes(scim, id);
      case RouteScimUserGet:
         return ScimGetUser(scim, id, projection);
      case RouteScimUserCreate:
         return ScimCreateUser(scim, GetRequestBody_(request));
      case RouteScimUserReplace:
         return ScimReplaceUser(scim, id, GetRequestBody_(request), ifMatch);
      case RouteScimUserPatch:
         return ScimPatchUser(scim, id, GetRequestBody_(request), ifMatch);
      case RouteScimUserDelete:
         return ScimDeleteUser(scim, id, ifMatch);
      case RouteScimGroupGet:
         return ScimGetGroup(scim, id, projection);
      case RouteScimGroupCreate:
         return ScimCreateGroup(scim, GetRequestBody_(request));
      case RouteScimGroupReplace:
         return ScimReplaceGroup(scim, id, GetRequestBody_(request), ifMatch);
      case RouteScimGroupPatch:
         return ScimPatchGroup(scim, id, GetRequestBody_(request), ifMatch);
      case RouteScimGroupDelete:
         return ScimDeleteGroup(scim, id, ifMatch);
      default:
         break;
      }

      return ScimUnknownPath(method, route.name);
   }

   AnsiString
   RestApiServer::OpenApiScimPaths_()
   {
      static const char *paths =
         ",\"/scim/v2/ServiceProviderConfig\":{\"get\":{\"summary\":\"SCIM 2.0: what this server supports\",\"description\":\"RFC 7644 section 4, in application/scim+json (docs/Scim.md): patch true, bulk false, filter true with maxResults 200, changePassword true, sort false - sortBy and sortOrder are ignored, as RFC 7644 allows when sort is not supported - etag true, and one authentication scheme, oauthbearertoken: an API key whose role holds provision, presented as Authorization: Bearer. Every route under /scim/ answers an API key and nothing else: the administrator password, an account's password and a session are answered 401 in SCIM's error shape (a session's write without X-Requested-With is 403, from the listener's own check that comes first), a key whose role does not hold provision 403, and a key past its request budget 429 with Retry-After. The HTTP layer's own refusals - 400 for a malformed request, 408 for one too slow, 413 for a body over 64 KB - come before the path is read, and are the listener's JSON rather than SCIM's.\",\"responses\":{\"200\":{\"description\":\"The ServiceProviderConfig\"},\"401\":{\"description\":\"Not an API key\"},\"403\":{\"description\":\"The key's role does not hold provision\"},\"429\":{\"description\":\"The key's request budget is spent\"}}}},"
         "\"/scim/v2/Schemas\":{\"get\":{\"summary\":\"SCIM 2.0: the User and Group schemas, as far as this server keeps them\",\"description\":\"A ListResponse of the two schemas, each listing only the attributes this server keeps: userName, name (givenName, familyName, formatted), displayName, active, emails (value, type work or other, primary) and password for User; displayName and members for Group.\",\"responses\":{\"200\":{\"description\":\"The schemas\"}}}},"
         "\"/scim/v2/Schemas/{id}\":{\"get\":{\"summary\":\"SCIM 2.0: one schema, by its URN\",\"description\":\"urn:ietf:params:scim:schemas:core:2.0:User or urn:ietf:params:scim:schemas:core:2.0:Group, compared without regard to case.\",\"parameters\":[{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"description\":\"The schema's URN\",\"schema\":{\"type\":\"string\"}}],\"responses\":{\"200\":{\"description\":\"The schema\"},\"404\":{\"description\":\"No schema with that id\"}}}},"
         "\"/scim/v2/ResourceTypes\":{\"get\":{\"summary\":\"SCIM 2.0: User and Group\",\"description\":\"A ListResponse of the two resource types.\",\"responses\":{\"200\":{\"description\":\"The resource types\"}}}},"
         "\"/scim/v2/ResourceTypes/{id}\":{\"get\":{\"summary\":\"SCIM 2.0: one resource type\",\"description\":\"User or Group, compared without regard to case.\",\"parameters\":[{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"description\":\"User or Group\",\"schema\":{\"type\":\"string\"}}],\"responses\":{\"200\":{\"description\":\"The resource type\"},\"404\":{\"description\":\"No resource type with that id\"}}}},"
         "\"/scim/v2/Users\":{"
         "\"get\":{\"summary\":\"SCIM 2.0: the accounts\",\"description\":\"A ListResponse of Users - accounts, in the order they were made - confined to a confined key's domains. filter is one of userName eq, co or sw, externalId eq, emails.value eq (the address or an alias of it) and active eq true or false; any other filter, and any and or or, is 400 invalidFilter. startIndex from 1 (a smaller one is 1) and count from 0, at most 200 (the default); a startIndex or count that is not a whole number is 400. sortBy and sortOrder are ignored. attributes and excludedAttributes name top-level attributes; id, schemas and meta are always answered. A User: id and userName the address, externalId, name (givenName, familyName, formatted), displayName, active, emails - the address (type work, primary) and the aliases whose value it is (type other) - and meta (resourceType, location, version, a weak ETag of the representation; no created and no lastModified, which this server does not keep).\",\"parameters\":[{\"name\":\"filter\",\"in\":\"query\",\"schema\":{\"type\":\"string\"}},{\"name\":\"startIndex\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"}},{\"name\":\"count\",\"in\":\"query\",\"schema\":{\"type\":\"integer\"}},{\"name\":\"attributes\",\"in\":\"query\",\"schema\":{\"type\":\"string\"}},{\"name\":\"excludedAttributes\",\"in\":\"query\",\"schema\":{\"type\":\"string\"}}],\"responses\":{\"200\":{\"description\":\"A ListResponse\"},\"400\":{\"description\":\"invalidFilter, or startIndex or count not a whole number\"}}},"
         "\"post\":{\"summary\":\"SCIM 2.0: make an account\",\"description\":\"A User: userName (required, at a domain this server hosts and the key reaches - otherwise 400 invalidValue, one sentence for both), name, displayName (split at its last space when no name comes with it), active (default true), emails (the work or primary entry must be userName; every other entry becomes an alias, at a domain the key provisions, not a spam trap, and within the domain's limit of aliases), password (optional: the password policy and the breach check judge it, and an account made without one has a random password nobody is told) and externalId (at most 255 characters). An attribute of the core User schema this server does not keep, and an extension's, is accepted and not kept; any other is 400 invalidValue. An address an account, an alias or a list already has is 409 uniqueness. Everything is judged before the account is made; an alias the database then refuses removes the account and the aliases this request made, and nothing else. 201 with the User, Location and ETag.\",\"requestBody\":{\"content\":{\"application/scim+json\":{\"schema\":{\"type\":\"object\",\"required\":[\"userName\"],\"properties\":{\"userName\":{\"type\":\"string\"},\"externalId\":{\"type\":\"string\"},\"name\":{\"type\":\"object\",\"properties\":{\"givenName\":{\"type\":\"string\"},\"familyName\":{\"type\":\"string\"}}},\"displayName\":{\"type\":\"string\"},\"active\":{\"type\":\"boolean\"},\"password\":{\"type\":\"string\",\"writeOnly\":true},\"emails\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"string\"},\"type\":{\"type\":\"string\"},\"primary\":{\"type\":\"boolean\"}}}}}}}}},\"responses\":{\"201\":{\"description\":\"The User\"},\"400\":{\"description\":\"invalidValue or invalidSyntax, with the sentence\"},\"409\":{\"description\":\"uniqueness: the address is taken\"},\"503\":{\"description\":\"The breach check could not be asked and is set to refuse\"}}}},"
         "\"/scim/v2/Users/{id}\":{"
         "\"get\":{\"summary\":\"SCIM 2.0: one account\",\"description\":\"An account at a domain a confined key does not reach is the same 404 as one that is not there. ETag carries the version.\",\"parameters\":[{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"description\":\"The account's address\",\"schema\":{\"type\":\"string\"}}],\"responses\":{\"200\":{\"description\":\"The User\"},\"404\":{\"description\":\"No user with that id\"}}},"
         "\"put\":{\"summary\":\"SCIM 2.0: replace an account\",\"description\":\"The body a POST takes; an attribute it leaves out is left as it is, and emails, when given, is the whole set of aliases, so an alias it leaves out is removed. A new userName renames the account through the mailbox move - everything keyed on it follows, the old address is kept as an alias (answered in emails as other) and the id changes; a mailbox in use is 409, and a userName that is one of the account's own aliases takes that alias's place. A displayName with no name beside it is split into the first and last names only for an account that has neither; otherwise it is accepted and not kept. active false suspends the account; a suspension and a new password end the account's sessions and drop its live IMAP, POP3 and SMTP connections. The whole change is judged before anything is written - the aliases against their saves and the domain's limit, the password against the policy, the reuse history and the breach check - and a refusal writes nothing. A write that fails after the first puts back what the request wrote, except a rename that happened: the 500 then says where the account now is. If-Match is honoured (412). A key may not change an account whose authority it does not take in (403): for a key confined to named domains, an account that administers another domain or that reaches every domain.\",\"parameters\":[{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"description\":\"The account's address\",\"schema\":{\"type\":\"string\"}}],\"responses\":{\"200\":{\"description\":\"The User as it now is\"},\"400\":{\"description\":\"invalidValue, with the sentence - a password the policy or the breach check refuses among them\"},\"403\":{\"description\":\"The account holds authority the key does not\"},\"404\":{\"description\":\"No user with that id\"},\"409\":{\"description\":\"uniqueness, or the mailbox is in use\"},\"412\":{\"description\":\"If-Match names another version\"},\"500\":{\"description\":\"A write failed part way; the detail says what stands\"},\"503\":{\"description\":\"The breach check could not be asked and is set to refuse\"}}},"
         "\"patch\":{\"summary\":\"SCIM 2.0: change an account\",\"description\":\"A PatchOp. add and replace on active, userName (a rename), externalId, displayName, name, name.givenName, name.familyName, password and emails; remove on externalId, displayName, name, name.givenName and name.familyName; and these forms of emails: emails (add and replace; remove is refused, since the account's own address cannot be removed), emails[type eq \\\"work\\\"].value (the account's own address, which must be written as itself), emails[type eq \\\"other\\\"].value (an alias) and remove of emails[value eq \\\"...\\\"]. A remove of active, userName or password, or a replace of one of them with null, is 400 invalidValue - an account always has a switch, an address and a password - and refuses the whole PATCH. An operation with no path carries an object of paths and values, as Entra sends it. A path naming an attribute this server does not keep, or an extension's, is accepted and not kept; a path naming nothing is 400 invalidPath. The operations apply in order to the account as it will be, and the whole is judged and written as a PUT is: a refusal writes nothing. If-Match is honoured (412).\",\"parameters\":[{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"description\":\"The account's address\",\"schema\":{\"type\":\"string\"}}],\"responses\":{\"200\":{\"description\":\"The User as it now is\"},\"400\":{\"description\":\"invalidPath, invalidValue, noTarget or invalidSyntax, with the sentence\"},\"403\":{\"description\":\"The account holds authority the key does not\"},\"404\":{\"description\":\"No user with that id\"},\"409\":{\"description\":\"uniqueness, or the mailbox is in use\"},\"412\":{\"description\":\"If-Match names another version\"},\"500\":{\"description\":\"A write failed part way; the detail says what stands\"},\"503\":{\"description\":\"The breach check could not be asked and is set to refuse\"}}},"
         "\"delete\":{\"summary\":\"SCIM 2.0: remove an account\",\"description\":\"Its sessions are ended and its live connections dropped; then the account and its mailbox; then every alias emails answered for it, and every row of every distribution list that names its address. An account the database would not remove is 500 with everything left as it was; something naming the address that could not be removed after the account was is 500 naming it. If-Match is honoured (412).\",\"parameters\":[{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"description\":\"The account's address\",\"schema\":{\"type\":\"string\"}}],\"responses\":{\"204\":{\"description\":\"Removed\"},\"403\":{\"description\":\"The account holds authority the key does not\"},\"404\":{\"description\":\"No user with that id\"},\"412\":{\"description\":\"If-Match names another version\"},\"500\":{\"description\":\"The account, or something naming its address, could not be removed\"}}}},"
         "\"/scim/v2/Groups\":{"
         "\"get\":{\"summary\":\"SCIM 2.0: the distribution lists\",\"description\":\"A ListResponse of Groups, confined to a confined key's domains. filter is displayName eq; startIndex, count, attributes and excludedAttributes as for Users, and sortBy and sortOrder ignored. A Group: id and displayName the list's address, members its confirmed members as {value: address}. A managed list's requests still waiting and the addresses that left it are not members, and its membership query is neither answered nor changed.\",\"responses\":{\"200\":{\"description\":\"A ListResponse\"},\"400\":{\"description\":\"invalidFilter\"}}},"
         "\"post\":{\"summary\":\"SCIM 2.0: make a distribution list\",\"description\":\"displayName (required: the list's address, at a domain the key provisions) and members ([{value}]): each an e-mail address, neither the list's own address nor another distribution list's - a list is not taken as a member of another - and, for a key confined to named domains, one in them. The list is public, active and unmanaged, as the list route makes one. externalId is accepted and not kept. 201 with the Group.\",\"responses\":{\"201\":{\"description\":\"The Group\"},\"400\":{\"description\":\"invalidValue\"},\"409\":{\"description\":\"uniqueness\"}}}},"
         "\"/scim/v2/Groups/{id}\":{"
         "\"get\":{\"summary\":\"SCIM 2.0: one distribution list\",\"parameters\":[{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"description\":\"The distribution list's address\",\"schema\":{\"type\":\"string\"}}],\"responses\":{\"200\":{\"description\":\"The Group\"},\"404\":{\"description\":\"No group with that id\"}}},"
         "\"put\":{\"summary\":\"SCIM 2.0: replace a distribution list\",\"description\":\"displayName (a new one changes the list's address, inside its domain only, and the old address is not kept) and members, the whole set when given. A member added is a confirmed member, as an administrator's add is - a waiting or departed row for the address is confirmed, not doubled - and a member removed has its row deleted; a member added is judged as for POST. A key confined to named domains is judged only on the members it names: a member outside its domains that the body leaves out is left where it is, and one the body names is 400. If-Match is honoured (412).\",\"parameters\":[{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"description\":\"The distribution list's address\",\"schema\":{\"type\":\"string\"}}],\"responses\":{\"200\":{\"description\":\"The Group\"},\"400\":{\"description\":\"invalidValue\"},\"404\":{\"description\":\"No group with that id\"},\"409\":{\"description\":\"uniqueness\"},\"412\":{\"description\":\"If-Match names another version\"}}},"
         "\"patch\":{\"summary\":\"SCIM 2.0: change a distribution list\",\"description\":\"A PatchOp: add, replace and remove of members (remove with a value removes those, without one every member - for a key confined to named domains, every member in them), remove of members[value eq \\\"...\\\"], and replace of displayName. Members are judged as for PUT. If-Match is honoured (412).\",\"parameters\":[{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"description\":\"The distribution list's address\",\"schema\":{\"type\":\"string\"}}],\"responses\":{\"200\":{\"description\":\"The Group\"},\"400\":{\"description\":\"invalidPath, invalidValue or noTarget\"},\"404\":{\"description\":\"No group with that id\"},\"409\":{\"description\":\"uniqueness: the new displayName is taken\"},\"412\":{\"description\":\"If-Match names another version\"}}},"
         "\"delete\":{\"summary\":\"SCIM 2.0: remove a distribution list\",\"description\":\"The list, its members and its archive. If-Match is honoured (412).\",\"parameters\":[{\"name\":\"id\",\"in\":\"path\",\"required\":true,\"description\":\"The distribution list's address\",\"schema\":{\"type\":\"string\"}}],\"responses\":{\"204\":{\"description\":\"Removed\"},\"404\":{\"description\":\"No group with that id\"},\"412\":{\"description\":\"If-Match names another version\"}}}}";

      return AnsiString(paths);
   }
}
