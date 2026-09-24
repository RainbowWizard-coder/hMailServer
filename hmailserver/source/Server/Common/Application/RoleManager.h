// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <vector>
#include <map>

#include "../BO/Account.h"

namespace HM
{
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

      // Sending mail as an application: POST /api/v1/send (docs/SendApi.md).
      // Held by an API key's role, and used only together with the key's own
      // sender (SendFrom in the key store) - a key sends when its role holds
      // this AND it names a sender, and never as anybody else. The server
      // administrator holds it, as it holds everything; no other built-in role
      // does, and a custom role may be given it. Appended, not inserted: the
      // token is what hm_roles stores, and the position is the bit.
      SendThroughApi,

      // Provisioning over SCIM 2.0: /scim/v2 (docs/Scim.md). The organisation's
      // identity provider creating, renaming, suspending and removing accounts,
      // their aliases and the distribution lists. Held by an API key's role and
      // used by a key and nothing else - every other credential is refused
      // /scim/ before any permission is asked. The server administrator holds
      // it, as it holds everything; no other built-in role does, and a custom
      // role may be given it, which is how a key that provisions and does
      // nothing else is made. Appended, not inserted: the token is what
      // hm_roles stores, and the position is the bit.
      Provision,

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

      // A bit per RolePermission. RolePermission::Count is 24, so a 64-bit
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

      // An account's grant. When an assignment row names a role, that role and
      // nothing else - even a role that has come to hold nothing, so that an
      // account narrowed to a role is never widened back to its admin level;
      // otherwise DERIVED from AdminLevel, so that every administrator that
      // existed before roles did goes on having exactly the authority it had.
      static RoleGrant GrantForAccount(std::shared_ptr<const Account> account);

      // The grant an account WOULD hold if it were given this role in these
      // domains - an empty list read exactly as GrantForAccount reads it - so
      // that the authority an assignment hands out is judged before it is
      // written, not after.
      static RoleGrant GrantForAssignment(std::shared_ptr<const Account> account, const String &roleName, const String &domains);

      // True when holder's authority takes in the whole of other's: every
      // permission other holds, in every domain other reaches. The rule behind
      // two refusals that are one refusal: a credential may not hand out
      // authority it has not got, and a credential may not take over an account
      // - set its password, change it - whose authority it has not got. Without
      // the second, a help desk that may reset passwords in a domain could
      // reset a domain administrator's there and sign in as them. A grant that
      // administers nothing is taken in by every grant. AdministerServer takes
      // in everything only when it reaches every domain: held with a list of
      // domains - a full key issued for named domains - it is judged by the
      // list like any confined grant, and takes in nothing that reaches every
      // domain.
      static bool Encompasses(const RoleGrant &holder, const RoleGrant &other);

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

      // Removes a custom role, and nothing else. Refused - false, with the
      // sentence in error - for a name no role has, for a built-in role, and for
      // a role any account holds, whose assignments are never taken away to make
      // it deletable: an account with no assignment holds what its admin level
      // gives it, so an account narrowed from server administrator to the role
      // would be the server administrator again, with nothing said. holders is
      // how many accounts hold the role when that is the refusal - a holder
      // whose assignment was written between the count and the delete included,
      // since the foreign key refuses that delete and the holders are counted
      // again - and 0 every other way. A refusal writes nothing, the audit trail
      // included.
      static bool DeleteCustomRole(const String &name, String &error, int &holders);

      // The assignments.
      static std::vector<RoleAssignment> ListAssignments();
      static bool AssignmentForAccount(__int64 accountId, RoleAssignment &assignment);
      static bool Assign(__int64 accountId, const String &roleName, const String &domains, String &error);

      // Takes the account's role away. True, and nothing written, for an
      // account that holds none.
      static bool Unassign(__int64 accountId, String &error);

      // What an assignment with no list of domains reaches. serveradmin and
      // auditor are server-wide by definition - the one has always had every
      // domain, and the other exists to read all of them - so for those two an
      // empty list is every domain. Every other role, custom roles included, is
      // confined, and an empty list is the account's own domain: naming domains
      // is how an administrator widens a confined role, never how they narrow
      // it. The listings answer this alongside the stored list, so that no page
      // has to know the rule to show what an assignment reaches.
      static bool ReachesEveryDomainWhenUnlisted(const String &roleName);

      // What an assignment's stored list reaches: "*" is every domain, which is
      // how an administrator lets a confined role - a custom role holding a
      // server-wide permission, a domain administrator of the whole server - be
      // used by an account at all; empty is the rule above; anything else is
      // that list. Assignments only: a key's Domains line keeps reading "*" as
      // the name of no domain, as it always has.
      static bool AssignmentReachesEveryDomain(const String &roleName, const String &domains);

      // True when a grant is confined to no list of domains - the server
      // administrator, an unrestricted key, an auditor given no list. A
      // question some resources have to ask before they can answer at all: a
      // search that matches any address, or one queued message whose
      // recipients sit in any number of domains, has no domain to confine to,
      // so only a grant that reaches every domain may ask it.
      static bool ReachesEveryDomain(const RoleGrant &grant);

      // True for the two grants that are what "administrator" has always meant
      // on this server: serveradmin and domainadmin, the only grants that hold
      // AdministerServer or AdministerDomain (a custom role may hold neither).
      // The audit trail's actor kind is read from this, at both interfaces, so
      // that an account given a narrower role is recorded as the account it
      // is and not as an administrator because its AdminLevel column says one.
      // A LABEL, never a decision: nothing is allowed or refused on it.
      static bool IsAdministratorGrant(const RoleGrant &grant);

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

      // THE BACKUP.
      //
      // An assignment is carried with its account (AccountStores), naming its
      // role by name, so that a restore onto another server finds that server's
      // role of the same name. A built-in role is on every server. A custom role
      // is not - it is a row of this server's hm_roles - so an archive restored
      // onto a fresh server used to find no role of that name and drop every
      // assignment of it.
      //
      // XMLStore writes the custom roles into the archive as a server-wide
      // store of their own, <Roles>, beside the settings and the [Settings]
      // section, whenever the archive carries the accounts or the settings: the
      // accounts are what name the roles, and a role is configuration. Read from
      // the table, not from the copy above, so that a role this build does not
      // recognise a permission of still survives the archive; the restore judges
      // the permissions again. False when the table could not be read, which
      // fails the backup rather than writing an archive that says there are no
      // custom roles.
      //
      // XMLLoad puts them back, and BackupExecuter::StartRestore calls it BEFORE
      // the domains - and so before the assignments - are restored. It CREATES
      // the roles this server lacks and KEEPS the ones it has, whatever the
      // archive says about them: a role that is here already is this server's
      // administrator's decision, and every holder of it that is not in the
      // archive - an API key issued with Role=<name>, whose store no archive
      // carries; on a settings-only restore, every account holding it - would
      // otherwise be widened by a restore with nothing said and no actor to
      // record it under. A kept role whose permissions or description differ
      // from the archive's is named in the backup log with both, so that an
      // administrator who wants the archive's definition makes that change
      // deliberately. An archive with no <Roles> is one taken before this
      // existed, and restores nothing here. A role the restore refuses - a
      // built-in name, a name no role may have, no permission this server
      // knows - is named in the backup log and skipped, and the assignments
      // that name it are then dropped as below. False only when a role could
      // not be written; the log then names the roles already created.
      static bool XMLStore(XNode *backupNode);
      static bool XMLLoad(XNode *backupNode);

      // THE ADMIN LEVEL BEHIND AN ASSIGNMENT, ON RESTORE. Without a row an
      // account's grant is DERIVED from AdminLevel (see GrantForAccount), and
      // the archive carries AdminLevel beside the assignment - so an account
      // whose level says server administrator and which had been narrowed to
      // a help desk is the server administrator for as long as its level
      // stands with no row beside it. Account::XMLLoad therefore saves such an
      // account as an ordinary one, and AccountStores::XMLLoadAssignment calls
      // one of these two once it knows whether the row can be written.
      //
      // RestoreArchivedAdminLevel: the row is written, so the level the
      // archive carries goes back behind it, as it was on the server the
      // backup was taken from. False when it could not be written; the
      // account then holds its role and nothing more, and the log says so.
      //
      // RevokeUnrestoredRole: the row could not be placed - its role is in
      // neither the archive nor this server - and the level stays withheld
      // (and is made sure of, should the row not have been saved that way).
      // The backup log and the error log name the account, the role that was
      // missing and the level that has not been given back, so that an
      // administrator gives the authority back deliberately. False when the
      // account's level could not be written, which fails the restore rather
      // than leaving the wider authority in place. archivedLevel is the level
      // the archive carries, which the account object no longer shows.
      static bool RestoreArchivedAdminLevel(Account &account, Account::AdminLevel archivedLevel);
      static bool RevokeUnrestoredRole(Account &account, const String &roleName, Account::AdminLevel archivedLevel);

   private:

      // The row and the object, in that order, so that a level that could not
      // be written is not reported as changed. The account leaves the cache.
      static bool WriteAdminLevel_(Account &account, Account::AdminLevel level);

      // SaveCustomRole, saying which of the two ways it failed: refused is true
      // when the role itself was refused (its name, its permissions) and false
      // when the database could not be written.
      static bool SaveCustomRole_(const String &name, const String &description, const String &permissions, String &error, bool &refused);

      // DeleteCustomRole's two halves: how many accounts hold the role, read
      // from the table (false when it could not be read), and the sentence a
      // refusal carries, the same whether the count or the foreign key refused.
      static bool CountHolders_(const RoleRecord &record, int &holders);
      static String HeldRoleRefusal_(const RoleRecord &record, int holders);

      static unsigned __int64 BuiltInPermissions_(const String &roleName, bool &found);
      static AnsiString DescribeGrantFromSet_(unsigned __int64 set);
      static unsigned __int64 ParsePermissionList_(const String &list);
      static std::vector<String> SplitDomains_(const String &list);
      static RoleGrant ConfineAssignment_(RoleGrant grant, const String &roleName, const String &domains, const String &ownDomain);
      static bool IsDomainReached_(const RoleGrant &grant, const String &domainName);
      static void EnsureLoaded_();
   };
}
