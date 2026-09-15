// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// Who may administer what, as a resource. See RestApiServer.h and, for the
// decision itself, Common/Application/RoleManager.h.
//
//    GET    /api/v1/roles                        the roles, with their permissions
//    POST   /api/v1/roles                        make a custom role
//    GET    /api/v1/roles/{name}                 one role
//    PUT    /api/v1/roles/{name}                 change a custom role
//    DELETE /api/v1/roles/{name}                 remove a custom role
//    GET    /api/v1/role-assignments             who holds which role, where
//    PUT    /api/v1/role-assignments/{address}   give an account a role
//    DELETE /api/v1/role-assignments/{address}   take it away
//    GET    /api/v1/session                      what this credential is and may do
//    POST   /api/v1/accounts/{address}/password  reset a password
//    POST   /api/v1/accounts/{address}/unlock    let somebody back in
//
// The last three are here rather than beside the routes they resemble because
// they are the roles' reason for existing. A help desk exists to reset a
// password and to unlock an account; it can be allowed exactly that and no
// more only if those two are routes of their own rather than fields of the
// account update, which also carries the address, the quota and the admin
// level. And GET /api/v1/session is what lets a page leave out a view its
// role may not use rather than showing one whose every button answers 403.
//
// A REFUSAL IS NOT AN ORACLE. Every route here answers a 404 for an account it
// may not see with the same body as for an account that does not exist, and
// the domain check happens in Authorize_ before the handler runs at all - so
// "no such account" and "not yours" are indistinguishable from outside. That
// is the property a help desk confined to one domain makes valuable and a
// mistake here would quietly destroy: a 403 that meant "it exists, elsewhere"
// enumerates every mailbox on the server one request at a time.

#include "StdAfx.h"
#include "RestApiServer.h"
#include "Crypt.h"
#include "PasswordPolicy.h"
#include "PasswordHistory.h"
#include "Time.h"
#include "AccountLockout.h"
#include "../Application/IniFileSettings.h"
#include "../Application/RoleManager.h"
#include "../BO/Account.h"
#include "../Persistence/PersistentAccount.h"
#include "../../POP3/POP3Sessions.h"
#include "MessageTrace.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // The account an address names, or nothing. Never says which of the two
      // it was: see the note at the top of this file.
      std::shared_ptr<Account> FindAccount(const AnsiString &address)
      {
         std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());

         if (!PersistentAccount::ReadObject(account, String(address)) || account->GetID() == 0)
            return std::shared_ptr<Account>();

         return account;
      }
   }

   AnsiString
   RestApiServer::RoleJson_(const RoleRecord &record)
   {
      AnsiString item;
      item.Format("{\"name\":\"%hs\",\"description\":\"%hs\",\"builtin\":%hs,\"permissions\":\"%hs\"}",
         JsonEscape_(Utf8_(record.name)).c_str(),
         JsonEscape_(Utf8_(record.description)).c_str(),
         record.builtin ? "true" : "false",
         JsonEscape_(Utf8_(record.permissions)).c_str());
      return item;
   }

   HttpResponse
   RestApiServer::HandleRoleList_()
   {
      std::vector<RoleRecord> roles = RoleManager::ListRoles();

      AnsiString items;
      int count = 0;

      for (const RoleRecord &record : roles)
      {
         if (count > 0)
            items += ",";

         items += RoleJson_(record);
         count++;
      }

      // The permission vocabulary travels with the listing, so that an editor
      // building a form for a custom role does not have to carry a copy of it
      // that can go out of date with the server.
      AnsiString permissions;
      for (int index = 0; index < static_cast<int>(RolePermission::Count); index++)
      {
         if (index > 0)
            permissions += ",";

         permissions += "\"";
         permissions += RoleManager::PermissionName(static_cast<RolePermission>(index));
         permissions += "\"";
      }

      AnsiString body;
      body.Format("{\"count\":%d,\"roles\":[%hs],\"permissions\":[%hs]}",
                  count, items.c_str(), permissions.c_str());

      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleRoleGet_(const AnsiString &name)
   {
      RoleRecord record;

      if (!RoleManager::FindRole(String(name), record))
         return BuildResponse_(404, "{\"error\":\"no such role\"}");

      return BuildResponse_(200, RoleJson_(record));
   }

   HttpResponse
   RestApiServer::HandleRoleSave_(const AnsiString &name, const AnsiString &requestBody, bool creating)
   {
      AnsiString wanted = creating ? GetJsonStringValue_(requestBody, "name") : name;
      AnsiString description = GetJsonStringValue_(requestBody, "description");
      AnsiString permissions = GetJsonStringValue_(requestBody, "permissions");

      wanted.Trim();

      if (wanted.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"name is required\"}");

      RoleRecord existing;
      const bool exists = RoleManager::FindRole(String(wanted), existing);

      if (creating && exists)
         return BuildResponse_(409, "{\"error\":\"a role of that name exists\"}");

      if (!creating && !exists)
         return BuildResponse_(404, "{\"error\":\"no such role\"}");

      String error;

      if (!RoleManager::SaveCustomRole(String(wanted), String(description), String(permissions), error))
         return BuildResponse_(400, AnsiString("{\"error\":\"") + JsonEscape_(Utf8_(error)) + "\"}");

      RoleRecord saved;
      RoleManager::FindRole(String(wanted), saved);

      String logged;
      logged.Format(_T("RestApi: the role '%s' was %s. Permissions: %s."),
                    String(wanted).c_str(),
                    creating ? _T("created") : _T("changed"),
                    saved.permissions.c_str());
      LOG_APPLICATION(logged);

      return BuildResponse_(creating ? 201 : 200, RoleJson_(saved));
   }

   HttpResponse
   RestApiServer::HandleRoleDelete_(const AnsiString &name)
   {
      String error;

      if (!RoleManager::DeleteCustomRole(String(name), error))
      {
         RoleRecord record;

         if (!RoleManager::FindRole(String(name), record))
            return BuildResponse_(404, "{\"error\":\"no such role\"}");

         return BuildResponse_(400, AnsiString("{\"error\":\"") + JsonEscape_(Utf8_(error)) + "\"}");
      }

      LOG_APPLICATION("RestApi: the role '" + String(name) + "' was deleted, with every assignment of it.");

      return BuildResponse_(200, "{\"deleted\":true}");
   }

   HttpResponse
   RestApiServer::HandleRoleAssignmentList_()
   {
      std::vector<RoleAssignment> assignments = RoleManager::ListAssignments();

      AnsiString items;
      int count = 0;

      for (const RoleAssignment &assignment : assignments)
      {
         std::shared_ptr<Account> account = std::shared_ptr<Account>(new Account());

         // An assignment whose account has gone is not shown. The row goes
         // with the account through the foreign key; this is for the moment
         // between the delete and the reload.
         if (!PersistentAccount::ReadObject(account, assignment.account_id) || account->GetID() == 0)
            continue;

         AnsiString item;
         item.Format("{\"address\":\"%hs\",\"role\":\"%hs\",\"domains\":\"%hs\",\"every_domain\":%hs}",
            JsonEscape_(Utf8_(account->GetAddress())).c_str(),
            JsonEscape_(Utf8_(assignment.role_name)).c_str(),
            JsonEscape_(Utf8_(assignment.domains)).c_str(),
            assignment.domains.IsEmpty() && RoleManager::ReachesEveryDomainWhenUnlisted(assignment.role_name) ? "true" : "false");

         if (count > 0)
            items += ",";

         items += item;
         count++;
      }

      AnsiString body;
      body.Format("{\"count\":%d,\"assignments\":[%hs]}", count, items.c_str());

      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleRoleAssign_(const Caller &caller, const AnsiString &address, const AnsiString &requestBody)
   {
      AnsiString roleName = GetJsonStringValue_(requestBody, "role");
      AnsiString domains = GetJsonStringValue_(requestBody, "domains");

      roleName.Trim();
      domains.Trim();

      std::shared_ptr<Account> account = FindAccount(address);

      if (!account)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      if (roleName.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"role is required; DELETE removes an assignment\"}");

      RoleRecord record;

      if (!RoleManager::FindRole(String(roleName), record))
         return BuildResponse_(400, "{\"error\":\"no such role\"}");

      // A credential may not hand out authority it does not hold. Without this
      // a domain administrator with ManageRoles - which no custom role may
      // have, but the administrator password's own delegate might one day -
      // could make an account a server administrator and then sign in as it.
      // Checked against the grant the request came in on, which is the same
      // object Authorize_ decided with.
      const RoleGrant wouldBe = RoleManager::GrantForRole(String(roleName), std::vector<String>());

      if (!caller.grant.Holds(RolePermission::AdministerServer) &&
          (wouldBe.permissions & ~caller.grant.permissions) != 0)
      {
         return BuildResponse_(403, "{\"error\":\"that role holds permissions this credential does not, and a credential may not hand out authority it has not got\"}");
      }

      String error;

      if (!RoleManager::Assign(account->GetID(), String(roleName), String(domains), error))
         return BuildResponse_(400, AnsiString("{\"error\":\"") + JsonEscape_(Utf8_(error)) + "\"}");

      const String where = domains.IsEmpty() ? String() : String(_T(" in ")) + String(domains);

      String logged;
      logged.Format(_T("RestApi: %s was given the role '%s'%s."),
                    account->GetAddress().c_str(), String(roleName).c_str(), where.c_str());
      LOG_APPLICATION(logged);

      RoleAssignment assignment;
      RoleManager::AssignmentForAccount(account->GetID(), assignment);

      AnsiString body;
      body.Format("{\"address\":\"%hs\",\"role\":\"%hs\",\"domains\":\"%hs\",\"every_domain\":%hs}",
         JsonEscape_(Utf8_(account->GetAddress())).c_str(),
         JsonEscape_(Utf8_(assignment.role_name)).c_str(),
         JsonEscape_(Utf8_(assignment.domains)).c_str(),
         assignment.domains.IsEmpty() && RoleManager::ReachesEveryDomainWhenUnlisted(assignment.role_name) ? "true" : "false");

      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleRoleUnassign_(const Caller &caller, const AnsiString &address)
   {
      std::shared_ptr<Account> account = FindAccount(address);

      if (!account)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      RoleAssignment assignment;

      if (!RoleManager::AssignmentForAccount(account->GetID(), assignment))
         return BuildResponse_(404, "{\"error\":\"that account holds no role\"}");

      const RoleGrant held = RoleManager::GrantForRole(assignment.role_name, std::vector<String>());

      if (!caller.grant.Holds(RolePermission::AdministerServer) &&
          (held.permissions & ~caller.grant.permissions) != 0)
      {
         return BuildResponse_(403, "{\"error\":\"that role holds permissions this credential does not, and a credential may not take away authority it has not got\"}");
      }

      String error;

      if (!RoleManager::Unassign(account->GetID(), error))
         return BuildResponse_(400, AnsiString("{\"error\":\"") + JsonEscape_(Utf8_(error)) + "\"}");

      LOG_APPLICATION("RestApi: " + account->GetAddress() + " no longer holds the role '" + assignment.role_name + "'.");

      return BuildResponse_(200, "{\"removed\":true}");
   }

   HttpResponse
   RestApiServer::HandleSessionGet_(const Caller &caller)
   {
      // What this credential is and what it may do, which is all a page needs
      // in order to show only the views its holder can use. It repeats nothing
      // the caller did not present: the identity it authenticated with, the
      // role that identity resolves to, the permissions of that role, and the
      // domains it reaches.
      AnsiString domains;
      int count = 0;

      for (const String &domain : caller.grant.domains)
      {
         if (count > 0)
            domains += ",";

         domains += "\"";
         domains += JsonEscape_(Utf8_(domain));
         domains += "\"";
         count++;
      }

      AnsiString permissions;
      count = 0;

      for (int index = 0; index < static_cast<int>(RolePermission::Count); index++)
      {
         if (!caller.grant.Holds(static_cast<RolePermission>(index)))
            continue;

         if (count > 0)
            permissions += ",";

         permissions += "\"";
         permissions += RoleManager::PermissionName(static_cast<RolePermission>(index));
         permissions += "\"";
         count++;
      }

      const char *kind =
         caller.result == AuthenticatedAsAdministrator ? "administrator" :
         caller.result == AuthenticatedWithApiKey ? "apikey" : "account";

      AnsiString body;
      body.Format("{\"identity\":\"%hs\",\"credential\":\"%hs\",\"administrative\":%hs,"
                  "\"role\":\"%hs\",\"permissions\":[%hs],\"domains\":[%hs]}",
         JsonEscape_(caller.identity).c_str(),
         kind,
         caller.grant.administrative ? "true" : "false",
         JsonEscape_(Utf8_(caller.grant.name)).c_str(),
         permissions.c_str(),
         domains.c_str());

      return BuildResponse_(200, body);
   }

   HttpResponse
   RestApiServer::HandleAccountPasswordReset_(const Caller &caller, const AnsiString &address, const AnsiString &requestBody)
   {
      AnsiString password = GetJsonStringValue_(requestBody, "password");

      std::shared_ptr<Account> account = FindAccount(address);

      if (!account)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      // A server administrator's password is not a help desk's to reset: the
      // account it belongs to administers every domain, so resetting it is a
      // way to become the server administrator from a role that reaches one
      // domain. The rule is the one the account update already applies to
      // admin_level, said again here because this route is narrower and would
      // otherwise be the way round it.
      if (account->GetAdminLevel() == Account::ServerAdmin &&
          !caller.grant.Holds(RolePermission::AdministerServer))
      {
         return BuildResponse_(403, "{\"error\":\"a server administrator's password may only be reset by a credential that administers the whole server\"}");
      }

      if (password.IsEmpty())
         return BuildResponse_(400, "{\"error\":\"password must not be empty\"}");

      if (account->GetIsAD())
         return BuildResponse_(409, "{\"error\":\"this account authenticates against the directory; its password is not this server's to set\"}");

      // The policy, the reuse history, the old hash recorded before the new
      // one replaces it, the configured hash and the age clock: exactly what
      // InterfaceAccount::put_Password and the account update do, because a
      // password set by a help desk must be no weaker than one set anywhere
      // else.
      String policyFailure;

      if (!PasswordPolicy::IsAcceptable(account->GetAddress(), String(password), policyFailure))
      {
         LOG_APPLICATION("RestApi: Refused a password for account " + String(address) + ": " + policyFailure);
         return BuildResponse_(400, AnsiString("{\"error\":\"") + JsonEscape_(Utf8_(policyFailure)) + "\"}");
      }

      if (PasswordHistory::IsReuse(account, String(password)))
         return BuildResponse_(409, "{\"error\":\"This password has been used recently on this account. Choose one that has not.\"}");

      PasswordHistory::Record(account);

      const int preferredHashAlgorithm = IniFileSettings::Instance()->GetPreferredHashAlgorithm();
      account->SetPassword(Crypt::Instance()->EnCrypt(String(password), (Crypt::EncryptionType) preferredHashAlgorithm));
      account->SetPasswordEncryption(preferredHashAlgorithm);
      account->SetPasswordChanged(Time::GetCurrentDateTime());

      if (!PersistentAccount::SaveObject(account))
         return BuildResponse_(500, "{\"error\":\"the account could not be saved\"}");

      // The lockout goes with it. Somebody whose password has just been reset
      // because they could not get in must be able to get in, and leaving the
      // counter standing would send them back to the help desk.
      AccountLockout::Instance()->RecordSuccess(account->GetAddress());

      LOG_APPLICATION("RestApi: the password of " + account->GetAddress() + " was reset by " + String(caller.identity) + ".");

      return BuildResponse_(200, "{\"reset\":true}");
   }

   HttpResponse
   RestApiServer::HandleAccountUnlock_(const Caller &caller, const AnsiString &address)
   {
      std::shared_ptr<Account> account = FindAccount(address);

      if (!account)
         return BuildResponse_(404, "{\"error\":\"no such account\"}");

      // Both locks, because a person who cannot get to their mail does not
      // know which one they are behind: the failed-authentication lockout that
      // refuses the password, and the POP3 mailbox lock a dropped session
      // leaves behind - which is what InterfaceAccount::UnlockMailbox clears.
      AccountLockout::Instance()->RecordSuccess(account->GetAddress());
      POP3Sessions::Instance()->Unlock(account->GetID());

      LOG_APPLICATION("RestApi: " + account->GetAddress() + " was unlocked by " + String(caller.identity) + ".");

      return BuildResponse_(200, "{\"unlocked\":true}");
   }

   HttpResponse
   RestApiServer::HandleMessageTrace_(const Caller &caller, RouteKind kind, const AnsiString &identifier)
   {
      // The same three questions Application.GlobalObjects.MessageTrace answers
      // over COM, and the same most-recent-first limit. Authorize_ has already
      // refused the two server-wide ones to a credential confined to named
      // domains, and the address one to an address outside them.
      const int maximum = 500;
      std::vector<MessageTraceEvent> events;

      if (kind == RouteMessageTraceQueue)
      {
         __int64 queueId = strtoll(identifier.c_str(), nullptr, 10);
         events = MessageTrace::GetByQueueID(queueId);
      }
      else if (kind == RouteMessageTraceAddress)
      {
         const String address = String(identifier);

         if (address.Find(_T("@")) <= 0 || StringParser::ExtractDomain(address).IsEmpty())
            return BuildResponse_(400, "{\"error\":\"a whole address is required\"}");

         // The query matches the address as a substring. A credential confined
         // to named domains sees the events that name the address exactly,
         // because the substring also answers for user@example.test.elsewhere,
         // which is another customer; one that reaches every domain sees what
         // the Control Panel's search shows.
         const bool confined = !caller.domains.empty();

         for (const MessageTraceEvent &traceEvent : MessageTrace::Search(address, maximum))
         {
            if (!confined ||
                traceEvent.sender.CompareNoCase(address.c_str()) == 0 ||
                traceEvent.recipient.CompareNoCase(address.c_str()) == 0)
            {
               events.push_back(traceEvent);
            }
         }
      }
      else
      {
         events = MessageTrace::Search(String(), maximum);
      }

      AnsiString items;
      int count = 0;

      for (const MessageTraceEvent &traceEvent : events)
      {
         AnsiString item;
         item.Format("{\"id\":%I64d,\"queue_id\":%I64d,\"occurred\":\"%hs\",\"event\":\"%hs\",\"sender\":\"%hs\","
                     "\"recipient\":\"%hs\",\"source_ip\":\"%hs\",\"status_code\":%d,\"detail\":\"%hs\"}",
            traceEvent.id, traceEvent.queue_id,
            JsonEscape_(Utf8_(traceEvent.occurred)).c_str(),
            JsonEscape_(Utf8_(traceEvent.event_name)).c_str(),
            JsonEscape_(Utf8_(traceEvent.sender)).c_str(),
            JsonEscape_(Utf8_(traceEvent.recipient)).c_str(),
            JsonEscape_(Utf8_(traceEvent.source_ip)).c_str(),
            traceEvent.status_code,
            JsonEscape_(Utf8_(traceEvent.detail)).c_str());

         if (count > 0)
            items += ",";

         items += item;
         count++;
      }

      AnsiString body;
      body.Format("{\"count\":%d,\"events\":[%hs]}", count, items.c_str());

      return BuildResponse_(200, body);
   }

   AnsiString
   RestApiServer::OpenApiRolesPaths_()
   {
      return
         ",\"/api/v1/roles\":{"
         "\"get\":{\"summary\":\"The roles, and what each may do\",\"description\":\"Every role with its permissions, and the whole permission vocabulary this server knows. The four built-in roles - serveradmin, domainadmin, helpdesk, auditor - have their permissions in the code and cannot be edited; a custom role's are in the database. Readable by any role that holds view_configuration, an auditor included: who may administer what is not a secret, and an audit that cannot see it is not an audit.\",\"responses\":{\"200\":{\"description\":\"count, roles (name, description, builtin, permissions), permissions (the vocabulary)\"}}},"
         "\"post\":{\"summary\":\"Make a custom role\",\"description\":\"Body: name (lower-case letters, digits, underscore and hyphen), description, permissions (comma-separated tokens from the vocabulary). A custom role may never hold administer_server, administer_domain or manage_roles - each of those is a way to write 'server administrator' without calling it that - and a token this server does not know is dropped rather than refused, so a role always ends with less authority than it asked for and never with more. Needs manage_roles.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"name\",\"permissions\"],\"properties\":{\"name\":{\"type\":\"string\"},\"description\":{\"type\":\"string\"},\"permissions\":{\"type\":\"string\"}}}}}},\"responses\":{\"201\":{\"description\":\"The role\"},\"400\":{\"description\":\"A bad name, a built-in name, or no permission this server has\"},\"409\":{\"description\":\"A role of that name exists\"}}}},"
         "\"/api/v1/roles/{name}\":{"
         "\"get\":{\"summary\":\"One role\",\"responses\":{\"200\":{\"description\":\"The role\"},\"404\":{\"description\":\"No such role\"}}},"
         "\"put\":{\"summary\":\"Change a custom role\",\"description\":\"Body: description and permissions, judged as the create judges them. A built-in role is refused: its permissions are in the code so that an UPDATE against the database cannot hand somebody a permission.\",\"responses\":{\"200\":{\"description\":\"The role\"},\"400\":{\"description\":\"A built-in role, or no permission this server has\"},\"404\":{\"description\":\"No such role\"}}},"
         "\"delete\":{\"summary\":\"Remove a custom role\",\"description\":\"Its assignments go with it, so nobody is left holding a role that does not exist. A built-in role is refused.\",\"responses\":{\"200\":{\"description\":\"Removed\"},\"400\":{\"description\":\"A built-in role\"},\"404\":{\"description\":\"No such role\"}}}},"
         "\"/api/v1/role-assignments\":{"
         "\"get\":{\"summary\":\"Who holds which role, and where\",\"description\":\"One entry per account with an explicit assignment: address, role, domains as stored, and every_domain - true when the list is empty and the role is serveradmin or auditor, which reach every domain unless told otherwise; every other role, custom roles included, reaches the account's own domain when the list is empty. An account with no entry holds the role its AdminLevel has always given it - server administrator, domain administrator or neither - which is why an upgrade writes no rows and nobody's authority changes on the day roles arrive.\",\"responses\":{\"200\":{\"description\":\"count, assignments\"}}}},"
         "\"/api/v1/role-assignments/{address}\":{"
         "\"put\":{\"summary\":\"Give an account a role\",\"description\":\"Body: role (a name from /api/v1/roles) and domains (comma-separated; empty means every domain for serveradmin and auditor and the account's own domain for every other role). A credential may not give away a permission it does not itself hold. Needs manage_roles, and is scoped to the address's domain.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"role\"],\"properties\":{\"role\":{\"type\":\"string\"},\"domains\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"address, role, domains, every_domain\"},\"400\":{\"description\":\"No role named, or no such role\"},\"403\":{\"description\":\"The role holds permissions this credential does not\"},\"404\":{\"description\":\"No such account\"}}},"
         "\"delete\":{\"summary\":\"Take a role away\",\"description\":\"The account falls back to the authority its AdminLevel gives it. Needs manage_roles.\",\"responses\":{\"200\":{\"description\":\"Removed\"},\"403\":{\"description\":\"The role holds permissions this credential does not\"},\"404\":{\"description\":\"No such account, or it holds no role\"}}}},"
         "\"/api/v1/accounts/{address}/password\":{"
         "\"post\":{\"summary\":\"Reset an account's password\",\"description\":\"Body: password. The password policy, the reuse history, the configured hash and the age clock, exactly as when the Control Panel sets one; the failed-authentication lockout is cleared with it, because somebody whose password has just been reset must be able to get in. A directory-linked account is 409. A server administrator's password may only be reset by a credential that administers the whole server. Needs reset_password, and is scoped to the address's domain - which is the whole of what the help-desk role is.\",\"requestBody\":{\"content\":{\"application/json\":{\"schema\":{\"type\":\"object\",\"required\":[\"password\"],\"properties\":{\"password\":{\"type\":\"string\"}}}}}},\"responses\":{\"200\":{\"description\":\"Reset\"},\"400\":{\"description\":\"Empty, or the policy refused it (the reason is in error)\"},\"403\":{\"description\":\"A server administrator's account\"},\"404\":{\"description\":\"No such account\"},\"409\":{\"description\":\"A directory-linked account, or a recently used password\"}}}},"
         "\"/api/v1/accounts/{address}/unlock\":{"
         "\"post\":{\"summary\":\"Let an account back in\",\"description\":\"Both locks: the failed-authentication lockout that refuses the password, and the POP3 mailbox lock a dropped session leaves behind. Needs unlock_account, and is scoped to the address's domain.\",\"responses\":{\"200\":{\"description\":\"Unlocked\"},\"404\":{\"description\":\"No such account\"}}}},"
         "\"/api/v1/message-trace\":{"
         "\"get\":{\"summary\":\"The most recent message trace events, for anyone\",\"description\":\"What Application.GlobalObjects.MessageTrace.Search answers for an empty address: the latest events, newest first, 500 at most. Server-wide, so refused to a credential confined to named domains. Needs view_message_trace.\",\"responses\":{\"200\":{\"description\":\"count, events (id, queue_id, occurred, event, sender, recipient, source_ip, status_code, detail)\"}}}},"
         "\"/api/v1/message-trace/address/{address}\":{"
         "\"get\":{\"summary\":\"Where mail to or from one address went\",\"description\":\"The events that name the address as sender or recipient, newest first, 500 at most. Scoped to the address's domain: a help desk confined to example.test traces user@example.test and nobody in another domain, and sees the events that name the address exactly - the search matches as a substring, which would otherwise also answer for another customer's user@example.test.elsewhere. Needs view_message_trace.\",\"responses\":{\"200\":{\"description\":\"count, events\"},\"400\":{\"description\":\"Not a whole address\"}}}},"
         "\"/api/v1/message-trace/queue/{id}\":{"
         "\"get\":{\"summary\":\"The story of one queued message\",\"description\":\"Every event recorded for one queue id, oldest first. One message carries recipients in any number of domains, so this is server-wide and refused to a credential confined to named domains. Needs view_message_trace.\",\"responses\":{\"200\":{\"description\":\"count, events\"}}}}";
   }
}
