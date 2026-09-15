// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <vector>
#include <map>

namespace HM
{
   class Account;

   // Who may administer what.
   //
   // Until September 2026 this server had two answers to that question and no
   // third: the server administrator (one password in hMailServer.ini, full
   // authority over everything) and the domain administrator (an account with
   // AdminLevel, full authority inside its own domain). An API key could be
   // narrowed to "read-only" and to a list of domains, which was a role system
   // in miniature living in one file and reaching one interface.
   //
   // A named role is what an admin console has instead: a small, closed set of
   // PERMISSIONS, a handful of roles that hold some of them, and an assignment
   // that says which role a principal holds and in which domains. The four
   // built-in roles are:
   //
   //   serveradmin   every permission, every domain. What the administrator
   //                 password has always been, and what an account with
   //                 AdminLevel = ServerAdmin has always been.
   //   domainadmin   the accounts, mailboxes and quarantine of named domains.
   //                 What an account with AdminLevel = DomainAdmin has always
   //                 been, confined - as it always was - to its own domain
   //                 unless the assignment names others.
   //   helpdesk      may reset a password, unlock an account and read a message
   //                 trace, in named domains. Nothing else: not a setting, not
   //                 a domain, not a mailbox's contents.
   //   auditor       may read everything the administration interfaces expose,
   //                 including the audit trail, and may change nothing at all.
   //                 This is exactly what a read-only API key has always been,
   //                 which is why a key with Scope=readonly resolves to it.
   //
   // THE CHOKE POINT. Every administrative decision in this server is
   // RoleManager::Allows(grant, permission, domain, reason) and nothing else
   // decides. build/check-role-choke-point.py fails the build when any other
   // file calls it, when a REST route is added that nobody classified, and when
   // the classification and the read-only rule disagree. The rule is the same
   // one ACLManager holds for folder access (build/check-authz-choke-point.py)
   // and it exists for the same reason: a mail server that answers "may they?"
   // in two places will answer it differently in one of them.
   //
   // WHAT IS DELIBERATELY NOT A PERMISSION. Reading a secret - a password hash,
   // a private key, the OAuth2 HMAC secret, the hash of an API key - stays with
   // AdministerServer and is not part of any View permission. An auditor that
   // could read a credential store is not read-only: it is one offline attack
   // away from being every role at once.
   //
   // ADDITIVE BY CONSTRUCTION. A principal with no assignment row keeps exactly
   // the authority it had before this existed: the grant is DERIVED from
   // Account::AdminLevel (see GrantForAccount). No upgrade has to write a row
   // for the existing administrators to go on working, and none does; the rows
   // exist for the roles that could not be expressed before.
   enum class RolePermission
   {
      // The two historical levels, spelled as permissions so that the two
      // questions COM has always asked - GetIsServerAdmin, GetIsDomainAdmin -
      // become questions for this class rather than answers of their own.
      AdministerServer = 0,
      AdministerDomain,

      // Reads.
      ViewConfiguration,     // the server's and a domain's configuration, minus every secret
      ViewAccounts,          // accounts, aliases, lists, groups and their properties
      ViewMailbox,           // another account's folders and messages
      ViewQueue,
      ViewQuarantine,
      ViewArchive,
      ViewLogs,              // the server logs
      ViewMessageTrace,      // where one message went, and why
      ViewAudit,             // the audit trail

      // Writes.
      ManageAccounts,        // create, change and delete accounts, aliases, lists, groups
      ResetPassword,         // set an account's password, disable its second factor
      UnlockAccount,         // clear a lockout and the logon-failure count
      ManageMailbox,         // another account's folders, messages and folder rights
      ManageQueue,
      ManageQuarantine,
      ManageArchive,
      ManageSettings,        // every server setting, listener, route, certificate and list
      ManageDomains,         // which domains exist
      ManageServer,          // restart, reinitialise, back up, install an update, run a script
      ManageRoles,           // roles, their assignments, and the API keys

      Count
   };

   // The authority one credential carries. Copied out of the store when the
   // credential is authenticated and carried with the request from there, so
   // that a role edited half way through a request cannot change the answer
   // half way through it.
   struct RoleGrant
   {
      RoleGrant() : permissions(0), administrative(false) { }

      // The role's name as an administrator sees it: "serveradmin",
      // "domainadmin", "helpdesk", "auditor", or a custom one. Empty means no
      // administrative authority at all.
      String name;

      // A bit per RolePermission. RolePermission::Count is 22, so a 64-bit
      // word is the whole set with room to spare; a static assertion in the
      // .cpp holds that true.
      unsigned __int64 permissions;

      // The domains this grant reaches. Empty means every domain, which is
      // what the server administrator and an unrestricted API key have.
      std::vector<String> domains;

      // False for an ordinary account: the principal may sign in and read its
      // own mail and nothing else. True the moment any administrative role is
      // held, including the two historical levels.
      bool administrative;

      bool Holds(RolePermission permission) const;
   };

   // Who did it, for the audit trail. Installed at each interface - the REST
   // dispatcher and the COM authentication entry points - and read back by the
   // recorder, so that the actor and the authority are one object and cannot
   // describe two different people.
   struct RoleActor
   {
      RoleActor() : account_id(0), domain_id(0) { }

      // "administrator", "account:<address>" or "key:<id>". Never the secret.
      AnsiString identity;
      __int64 account_id;
      __int64 domain_id;
      RoleGrant grant;

      bool IsEmpty() const { return identity.IsEmpty(); }
   };

   // One role as it is stored and as it is listed.
   struct RoleRecord
   {
      RoleRecord() : id(0), builtin(false) { }

      __int64 id;
      String name;
      String description;
      bool builtin;
      // For a custom role: the permission tokens it holds, comma-separated.
      // For a built-in role this column is kept in step for anybody reading
      // the table by hand, and is IGNORED when the grant is built - a built-in
      // role's permissions come from the code below and from nowhere else, so
      // that an UPDATE against the database cannot hand somebody a permission.
      String permissions;
   };

   // One assignment: an account holds a role, in some domains.
   struct RoleAssignment
   {
      RoleAssignment() : id(0), account_id(0), role_id(0) { }

      __int64 id;
      __int64 account_id;
      __int64 role_id;
      String role_name;
      String address;   // filled in by the listing, for the editors
      String domains;   // comma-separated; empty means every domain the role reaches
   };

   class RoleManager
   {
   public:

      // THE DECISION. The only function in this server that answers "may this
      // credential do this, here". reason is filled in on a refusal with a
      // sentence a 403 can carry; it never names a resource the caller may not
      // see, because a refusal that distinguishes "no such account" from "not
      // yours" is an oracle for enumerating other people's mailboxes.
      static bool Allows(const RoleGrant &grant, RolePermission permission, const String &domainName, String &reason);

      // The same question with no domain in it: for the server-wide
      // permissions, and for the two historical levels COM asks about.
      static bool Allows(const RoleGrant &grant, RolePermission permission);

      // The grants.
      //
      // ServerAdministrator: the hMailServer.ini password, and the single-use
      // token the update helper redeems. Full authority, as it always was.
      static RoleGrant ServerAdministrator();

      // An account's grant. When an assignment row names a role, that role;
      // otherwise DERIVED from AdminLevel, so that every administrator that
      // existed before roles did goes on having exactly the authority it had.
      static RoleGrant GrantForAccount(std::shared_ptr<const Account> account);

      // An API key's grant. roleName is the key store's Role= line; when it is
      // empty the historical Scope= decides, exactly as it did: full is
      // serveradmin, read-only is auditor. domains is the key's Domains= line.
      static RoleGrant GrantForApiKey(const String &roleName, bool readOnly, const std::vector<String> &domains);

      // A named role's grant, with the domains an assignment confines it to.
      // An unknown name yields a grant that holds nothing, which is what a
      // hand-edited row that names a role somebody deleted must come to.
      static RoleGrant GrantForRole(const String &roleName, const std::vector<String> &domains);

      // The role names and their permissions, for the editors and for the
      // openapi document. Built-in roles first, then the custom ones by name.
      static std::vector<RoleRecord> ListRoles();
      static bool FindRole(const String &name, RoleRecord &record);
      static bool SaveCustomRole(const String &name, const String &description, const String &permissions, String &error);
      static bool DeleteCustomRole(const String &name, String &error);

      // The assignments.
      static std::vector<RoleAssignment> ListAssignments();
      static bool AssignmentForAccount(__int64 accountId, RoleAssignment &assignment);
      static bool Assign(__int64 accountId, const String &roleName, const String &domains, String &error);
      static bool Unassign(__int64 accountId, String &error);

      // The name of a permission, and back again. The tokens are what the
      // database, the REST document and the two editors all spell.
      static AnsiString PermissionName(RolePermission permission);
      static bool ParsePermission(const AnsiString &token, RolePermission &permission);
      static AnsiString DescribeGrant(const RoleGrant &grant);

      // Who is acting, on this thread. The audit trail reads this; nothing
      // else may decide from it (the decision takes a grant by value, so that
      // a record of who did something can never become a second answer to
      // whether they might).
      static void SetCurrentActor(const RoleActor &actor);
      static void ClearCurrentActor();
      static RoleActor CurrentActor();

      // Installs an actor for the length of a scope and takes it away again,
      // however the scope is left. One of these at each interface is what
      // makes the thread-local safe on a pooled thread.
      class ActorScope
      {
      public:
         explicit ActorScope(const RoleActor &actor) { RoleManager::SetCurrentActor(actor); }
         ~ActorScope() { RoleManager::ClearCurrentActor(); }
      private:
         ActorScope(const ActorScope &);
         ActorScope &operator=(const ActorScope &);
      };

      // Read once at start-up, so that the decision is a map lookup and not a
      // query. Called again by every write below.
      static void Reload();

   private:

      static unsigned __int64 BuiltInPermissions_(const String &roleName, bool &found);
      static AnsiString DescribeGrantFromSet_(unsigned __int64 set);
      static unsigned __int64 ParsePermissionList_(const String &list);
      static std::vector<String> SplitDomains_(const String &list);
      static bool IsDomainReached_(const RoleGrant &grant, const String &domainName);
      static void EnsureLoaded_();
   };
}
