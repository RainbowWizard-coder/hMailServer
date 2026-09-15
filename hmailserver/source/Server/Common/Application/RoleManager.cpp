// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

// See RoleManager.h. This file holds the whole decision and the whole store:
// the permission names, the four built-in roles, the grant each kind of
// credential resolves to, the one function that answers "may they", and the
// reads and writes of hm_roles and hm_roleassignments.
//
// It is one translation unit on purpose. build/check-role-choke-point.py can
// then state its rule as a sentence about file names - "RoleManager::Allows is
// named in RoleManager.cpp, in the REST choke point and in the COM one, and
// nowhere else" - which is a rule a reviewer can hold in their head and a
// script can prove.

#include "StdAfx.h"

#include "RoleManager.h"

#include "Application.h"
#include "../BO/Account.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DALRecordset.h"
#include "../Util/Parsing/StringParser.h"

#include <algorithm>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   static_assert(static_cast<int>(RolePermission::Count) <= 64,
                 "RoleGrant::permissions is one 64-bit word; a 65th permission needs a wider set, not a wrapped bit");

   namespace
   {
      // The token for each permission, in the enum's order. These strings are
      // the contract: they are what hm_roles.rolepermissions holds, what the
      // REST document answers, and what both editors show. Renaming one is a
      // schema change, not a caption change.
      const char *const PermissionTokens[] =
      {
         "administer_server",
         "administer_domain",
         "view_configuration",
         "view_accounts",
         "view_mailbox",
         "view_queue",
         "view_quarantine",
         "view_archive",
         "view_logs",
         "view_message_trace",
         "view_audit",
         "manage_accounts",
         "reset_password",
         "unlock_account",
         "manage_mailbox",
         "manage_queue",
         "manage_quarantine",
         "manage_archive",
         "manage_settings",
         "manage_domains",
         "manage_server",
         "manage_roles"
      };

      static_assert(sizeof(PermissionTokens) / sizeof(PermissionTokens[0]) == static_cast<size_t>(RolePermission::Count),
                    "a permission was added to the enum without a token; the token is what the database stores");

      unsigned __int64 Bit(RolePermission permission)
      {
         return 1ULL << static_cast<int>(permission);
      }

      // THE FOUR BUILT-IN ROLES. A built-in role's permissions come from here
      // and from nowhere else - never from hm_roles.rolepermissions - so that
      // an UPDATE against the database cannot hand somebody a permission the
      // code never gave their role. The column is kept in step for anybody
      // reading the table, and ignored when the grant is built.
      //
      // build/check-role-choke-point.py parses these four functions, so keep
      // one permission per line in the shape "set |= Bit(RolePermission::X);".

      unsigned __int64 ServerAdminPermissions()
      {
         // Everything, including the reads of secrets that no View permission
         // covers. This is what the hMailServer.ini password has always been.
         unsigned __int64 set = 0;
         for (int index = 0; index < static_cast<int>(RolePermission::Count); index++)
            set |= 1ULL << index;
         return set;
      }

      unsigned __int64 DomainAdminPermissions()
      {
         // Exactly what an account with AdminLevel = DomainAdmin has been able
         // to do since 2007, written down: its domain's accounts, aliases,
         // lists and mailboxes, and the quarantine entries that name them. Not
         // a server setting, not a listener, not another domain, not the logs -
         // which carry every domain's traffic - and not the roles.
         unsigned __int64 set = 0;
         set |= Bit(RolePermission::AdministerDomain);
         set |= Bit(RolePermission::ViewConfiguration);
         set |= Bit(RolePermission::ViewAccounts);
         set |= Bit(RolePermission::ViewMailbox);
         set |= Bit(RolePermission::ViewQuarantine);
         set |= Bit(RolePermission::ViewMessageTrace);
         set |= Bit(RolePermission::ManageAccounts);
         set |= Bit(RolePermission::ResetPassword);
         set |= Bit(RolePermission::UnlockAccount);
         set |= Bit(RolePermission::ManageMailbox);
         set |= Bit(RolePermission::ManageQuarantine);
         return set;
      }

      unsigned __int64 HelpdeskPermissions()
      {
         // The row asks for exactly four things and this is the whole of it: a
         // password reset, an unlock, a message trace, and enough of a read to
         // find the account the caller was rung up about. Deliberately no
         // ViewMailbox: a help desk that can open the mail it is resetting the
         // password for is not a help desk.
         unsigned __int64 set = 0;
         set |= Bit(RolePermission::ViewAccounts);
         set |= Bit(RolePermission::ResetPassword);
         set |= Bit(RolePermission::UnlockAccount);
         set |= Bit(RolePermission::ViewMessageTrace);
         return set;
      }

      unsigned __int64 AuditorPermissions()
      {
         // Every read and no write. This set must remain exactly the View
         // permissions: check-role-choke-point.py proves that every REST route
         // this server calls non-mutating needs a permission the auditor holds
         // and every mutating one needs a permission it does not, which is what
         // makes "an auditor changes nothing" a property of the code rather
         // than a claim about it.
         unsigned __int64 set = 0;
         set |= Bit(RolePermission::ViewConfiguration);
         set |= Bit(RolePermission::ViewAccounts);
         set |= Bit(RolePermission::ViewMailbox);
         set |= Bit(RolePermission::ViewQueue);
         set |= Bit(RolePermission::ViewQuarantine);
         set |= Bit(RolePermission::ViewArchive);
         set |= Bit(RolePermission::ViewLogs);
         set |= Bit(RolePermission::ViewMessageTrace);
         set |= Bit(RolePermission::ViewAudit);
         return set;
      }

      // The store, read once and kept. An administrative decision happens on
      // every request of every interface; a query for it would put the
      // database in the path of the answer, and a database that is briefly
      // away would then read as a loss of authority rather than as what it is.
      boost::recursive_mutex load_mutex;
      bool loaded = false;
      std::map<String, RoleRecord> roles_by_name;      // key: lower-case name
      std::map<__int64, RoleAssignment> assignment_by_account;

      String Lower(const String &value)
      {
         String copy = value;
         copy.MakeLower();
         return copy;
      }

      // One actor per thread. Non-trivial and thread_local on purpose: the
      // listeners run on long-lived pooled threads, so this is one object per
      // thread for the life of the process and never a per-request allocation.
      thread_local RoleActor current_actor;
   }

   bool
   RoleGrant::Holds(RolePermission permission) const
   {
      return (permissions & (1ULL << static_cast<int>(permission))) != 0;
   }

   AnsiString
   RoleManager::PermissionName(RolePermission permission)
   {
      const int index = static_cast<int>(permission);
      if (index < 0 || index >= static_cast<int>(RolePermission::Count))
         return "";

      return PermissionTokens[index];
   }

   bool
   RoleManager::ParsePermission(const AnsiString &token, RolePermission &permission)
   {
      AnsiString wanted = token;
      wanted.Trim();
      wanted.MakeLower();

      for (int index = 0; index < static_cast<int>(RolePermission::Count); index++)
      {
         if (wanted.Compare(PermissionTokens[index]) == 0)
         {
            permission = static_cast<RolePermission>(index);
            return true;
         }
      }

      return false;
   }

   AnsiString
   RoleManager::DescribeGrant(const RoleGrant &grant)
   {
      AnsiString result;

      for (int index = 0; index < static_cast<int>(RolePermission::Count); index++)
      {
         if ((grant.permissions & (1ULL << index)) == 0)
            continue;

         if (!result.IsEmpty())
            result += ",";
         result += PermissionTokens[index];
      }

      return result;
   }

   unsigned __int64
   RoleManager::BuiltInPermissions_(const String &roleName, bool &found)
   {
      found = true;

      const String name = Lower(roleName);

      if (name.Compare(_T("serveradmin")) == 0)
         return ServerAdminPermissions();
      if (name.Compare(_T("domainadmin")) == 0)
         return DomainAdminPermissions();
      if (name.Compare(_T("helpdesk")) == 0)
         return HelpdeskPermissions();
      if (name.Compare(_T("auditor")) == 0)
         return AuditorPermissions();

      found = false;
      return 0;
   }

   unsigned __int64
   RoleManager::ParsePermissionList_(const String &list)
   {
      unsigned __int64 set = 0;

      std::vector<String> tokens = StringParser::SplitString(list, _T(","));
      for (const String &token : tokens)
      {
         RolePermission permission = RolePermission::Count;
         if (ParsePermission(AnsiString(token), permission))
            set |= Bit(permission);

         // A token nobody recognises is silently nothing, deliberately: a
         // hand-edited row that names a permission this version does not have
         // must leave the role with less authority than it asked for, never
         // with more, and never with none of the permissions it spelled
         // correctly either.
      }

      // A custom role may not hold the two historical levels or the roles
      // themselves. Either would be a way to write "serveradmin" without
      // calling it that: AdministerServer IS the full authority, and
      // ManageRoles is the permission that can hand out AdministerServer.
      set &= ~Bit(RolePermission::AdministerServer);
      set &= ~Bit(RolePermission::AdministerDomain);
      set &= ~Bit(RolePermission::ManageRoles);

      return set;
   }

   std::vector<String>
   RoleManager::SplitDomains_(const String &list)
   {
      std::vector<String> result;

      for (const String &entry : StringParser::SplitString(list, _T(",")))
      {
         String trimmed = entry;
         trimmed.Trim();
         trimmed.MakeLower();

         if (!trimmed.IsEmpty())
            result.push_back(trimmed);
      }

      return result;
   }

   void
   RoleManager::EnsureLoaded_()
   {
      boost::lock_guard<boost::recursive_mutex> guard(load_mutex);

      if (loaded)
         return;

      Reload();
   }

   void
   RoleManager::Reload()
   {
      boost::lock_guard<boost::recursive_mutex> guard(load_mutex);

      std::map<String, RoleRecord> freshRoles;
      std::map<__int64, RoleAssignment> freshAssignments;

      SQLCommand selectRoles("select roleid, rolename, roledescription, rolebuiltin, rolepermissions from hm_roles");
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(selectRoles);

      if (!recordset)
      {
         // No database yet, or one that is briefly away. Leave whatever was
         // loaded in place and do not mark the store loaded, so that the next
         // decision tries again. The built-in roles are in the code, so the
         // server administrator and the historical domain administrator go on
         // working through the whole of an outage.
         return;
      }

      while (!recordset->IsEOF())
      {
         RoleRecord record;
         record.id = recordset->GetInt64Value("roleid");
         record.name = recordset->GetStringValue("rolename");
         record.description = recordset->GetStringValue("roledescription");
         record.builtin = recordset->GetLongValue("rolebuiltin") != 0;
         record.permissions = recordset->GetStringValue("rolepermissions");

         freshRoles[Lower(record.name)] = record;
         recordset->MoveNext();
      }

      SQLCommand selectAssignments(
         "select raid, raaccountid, raroleid, radomains, rolename from hm_roleassignments "
         "inner join hm_roles on raroleid = roleid");
      std::shared_ptr<DALRecordset> assignments = Application::Instance()->GetDBManager()->OpenRecordset(selectAssignments);

      if (!assignments)
         return;

      while (!assignments->IsEOF())
      {
         RoleAssignment assignment;
         assignment.id = assignments->GetInt64Value("raid");
         assignment.account_id = assignments->GetInt64Value("raaccountid");
         assignment.role_id = assignments->GetInt64Value("raroleid");
         assignment.domains = assignments->GetStringValue("radomains");
         assignment.role_name = assignments->GetStringValue("rolename");

         freshAssignments[assignment.account_id] = assignment;
         assignments->MoveNext();
      }

      roles_by_name.swap(freshRoles);
      assignment_by_account.swap(freshAssignments);
      loaded = true;
   }

   RoleGrant
   RoleManager::ServerAdministrator()
   {
      RoleGrant grant;
      grant.name = _T("serveradmin");
      grant.permissions = ServerAdminPermissions();
      grant.administrative = true;
      // No domains: every one.
      return grant;
   }

   RoleGrant
   RoleManager::GrantForRole(const String &roleName, const std::vector<String> &domains)
   {
      RoleGrant grant;

      if (roleName.IsEmpty())
         return grant;

      bool builtin = false;
      const unsigned __int64 fromCode = BuiltInPermissions_(roleName, builtin);

      if (builtin)
      {
         grant.name = Lower(roleName);
         grant.permissions = fromCode;
         grant.domains = domains;
         grant.administrative = true;
         return grant;
      }

      EnsureLoaded_();

      boost::lock_guard<boost::recursive_mutex> guard(load_mutex);

      auto found = roles_by_name.find(Lower(roleName));
      if (found == roles_by_name.end())
      {
         // A role that is not in the table and is not one of the four. This is
         // what a deleted role, or a row somebody typed by hand, looks like:
         // it holds nothing, so the principal falls all the way back to being
         // an ordinary account rather than quietly keeping what it had.
         return grant;
      }

      grant.name = Lower(found->second.name);
      grant.permissions = ParsePermissionList_(found->second.permissions);
      grant.domains = domains;
      grant.administrative = grant.permissions != 0;

      return grant;
   }

   bool
   RoleManager::ReachesEveryDomainWhenUnlisted(const String &roleName)
   {
      const String name = Lower(roleName);
      return name.Compare(_T("serveradmin")) == 0 || name.Compare(_T("auditor")) == 0;
   }

   RoleGrant
   RoleManager::GrantForAccount(std::shared_ptr<const Account> account)
   {
      RoleGrant grant;

      if (!account)
         return grant;

      EnsureLoaded_();

      RoleAssignment assignment;
      const bool assigned = AssignmentForAccount(account->GetID(), assignment);

      // The account's own domain is what an unqualified administrative role is
      // confined to. This is the historical behaviour of AdminLevel and it is
      // also the safe default for a new assignment: naming a domain is how an
      // administrator widens a role, never how they narrow it.
      const String ownDomain = Lower(StringParser::ExtractDomain(account->GetAddress()));

      if (assigned)
      {
         std::vector<String> domains = SplitDomains_(assignment.domains);

         RoleGrant assignedGrant = GrantForRole(assignment.role_name, domains);

         if (assignedGrant.administrative)
         {
            if (assignedGrant.domains.empty() && !ReachesEveryDomainWhenUnlisted(assignment.role_name))
            {
               // A confined role with no list of its own reaches the account's
               // own domain, and only that. serveradmin and auditor are the
               // exceptions: the one has never been a domain's administrator,
               // and an auditor confined to the domain its own mailbox happens
               // to sit in could not read the settings, the logs or the queue -
               // which are server-wide - and would not be reading everything.
               assignedGrant.domains.push_back(ownDomain);
            }

            return assignedGrant;
         }

         // The row named a role that no longer holds anything. Fall through to
         // the historical level rather than to nothing, so that deleting a
         // custom role cannot lock a server administrator out of their server.
      }

      switch (account->GetAdminLevel())
      {
      case Account::ServerAdmin:
         return ServerAdministrator();

      case Account::DomainAdmin:
      {
         RoleGrant derived = GrantForRole(_T("domainadmin"), std::vector<String>());
         derived.domains.push_back(ownDomain);
         return derived;
      }

      default:
         break;
      }

      // An ordinary account: not administrative, and that is the whole answer.
      return grant;
   }

   RoleGrant
   RoleManager::GrantForApiKey(const String &roleName, bool readOnly, const std::vector<String> &domains)
   {
      std::vector<String> lowered;
      for (const String &domain : domains)
         lowered.push_back(Lower(domain));

      if (!roleName.IsEmpty())
      {
         RoleGrant named = GrantForRole(roleName, lowered);
         if (named.administrative)
            return named;

         // A Role= line naming something this server does not have. Fail
         // closed on the NAME rather than falling back to the Scope, because a
         // key whose role was deleted must stop working rather than quietly
         // revert to the authority its scope alone would give.
         return RoleGrant();
      }

      // No Role= line: the scope that has always been there decides, and it
      // decides exactly what it always did. Scope=full is the server
      // administrator; Scope=readonly is the auditor, which is the same set of
      // reads a read-only key has had since August 2026 with the audit trail
      // added to it.
      RoleGrant grant = readOnly
         ? GrantForRole(_T("auditor"), lowered)
         : GrantForRole(_T("serveradmin"), lowered);

      grant.domains = lowered;
      return grant;
   }

   bool
   RoleManager::IsDomainReached_(const RoleGrant &grant, const String &domainName)
   {
      // No list means every domain, which is what the server administrator and
      // an unrestricted key have.
      if (grant.domains.empty())
         return true;

      if (domainName.IsEmpty())
         return false;

      for (const String &allowed : grant.domains)
      {
         if (allowed.CompareNoCase(domainName.c_str()) == 0)
            return true;
      }

      return false;
   }

   bool
   RoleManager::Allows(const RoleGrant &grant, RolePermission permission)
   {
      String reason;
      return Allows(grant, permission, String(), reason);
   }

   bool
   RoleManager::Allows(const RoleGrant &grant, RolePermission permission, const String &domainName, String &reason)
   {
      reason = _T("");

      if (!grant.administrative)
      {
         reason = _T("this credential does not administer this server");
         return false;
      }

      // AdministerServer is the whole of it, and is checked first so that the
      // server administrator's answer does not depend on any list below.
      const bool full = grant.Holds(RolePermission::AdministerServer);

      if (!full && !grant.Holds(permission))
      {
         // Names the permission and the role, and nothing about the resource.
         // A refusal that said "no such account" for one address and "not
         // yours" for another would let a help desk in one domain enumerate
         // every mailbox on the server one 403 at a time; both read the same
         // here, and the domain check below is written to keep it that way.
         reason.Format(_T("the role '%s' does not hold the permission '%s'"),
                       grant.name.c_str(), String(PermissionName(permission)).c_str());
         return false;
      }

      if (full)
         return true;

      // A decision with no domain in it - the server's status, its settings,
      // the queue, the logs - is finished: the permission was the whole of the
      // question, and a confined role that holds a server-wide permission
      // holds it everywhere. The confinement bites only where a domain is
      // actually named, which is exactly where somebody could otherwise reach
      // into another customer's mail by editing one segment of a path.
      if (domainName.IsEmpty())
         return true;

      if (!IsDomainReached_(grant, Lower(domainName)))
      {
         // Deliberately the same sentence whatever the reason the domain is
         // out of reach - it does not exist, it exists and is somebody else's,
         // the address was misspelled. The caller learns that its role does
         // not reach where it asked and nothing about what is there.
         reason.Format(_T("the role '%s' does not reach that domain"), grant.name.c_str());
         return false;
      }

      return true;
   }

   std::vector<RoleRecord>
   RoleManager::ListRoles()
   {
      EnsureLoaded_();

      boost::lock_guard<boost::recursive_mutex> guard(load_mutex);

      std::vector<RoleRecord> builtins;
      std::vector<RoleRecord> custom;

      for (const auto &entry : roles_by_name)
      {
         RoleRecord record = entry.second;

         if (record.builtin)
         {
            bool found = false;
            // Answer with what the code holds, never with what the column
            // says: the two are kept in step by the upgrade, and the code is
            // the one that decides.
            record.permissions = String(DescribeGrantFromSet_(BuiltInPermissions_(record.name, found)));
            builtins.push_back(record);
         }
         else
         {
            RoleGrant grant = GrantForRole(record.name, std::vector<String>());
            record.permissions = String(DescribeGrant(grant));
            custom.push_back(record);
         }
      }

      // Built-in roles first, in the order an administrator thinks of them.
      std::sort(builtins.begin(), builtins.end(), [](const RoleRecord &left, const RoleRecord &right)
      {
         return left.id < right.id;
      });

      for (const RoleRecord &record : custom)
         builtins.push_back(record);

      return builtins;
   }

   AnsiString
   RoleManager::DescribeGrantFromSet_(unsigned __int64 set)
   {
      RoleGrant grant;
      grant.permissions = set;
      return DescribeGrant(grant);
   }

   bool
   RoleManager::FindRole(const String &name, RoleRecord &record)
   {
      EnsureLoaded_();

      boost::lock_guard<boost::recursive_mutex> guard(load_mutex);

      auto found = roles_by_name.find(Lower(name));
      if (found == roles_by_name.end())
         return false;

      record = found->second;

      bool builtin = false;
      record.permissions = record.builtin
         ? String(DescribeGrantFromSet_(BuiltInPermissions_(record.name, builtin)))
         : String(DescribeGrant(GrantForRole(record.name, std::vector<String>())));

      return true;
   }

   bool
   RoleManager::SaveCustomRole(const String &name, const String &description, const String &permissions, String &error)
   {
      error = _T("");

      String wanted = name;
      wanted.Trim();
      wanted.MakeLower();

      if (wanted.IsEmpty())
      {
         error = _T("a role needs a name");
         return false;
      }

      for (int position = 0; position < wanted.GetLength(); position++)
      {
         const TCHAR character = wanted.GetAt(position);
         const bool acceptable =
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') ||
            character == '_' || character == '-';

         if (!acceptable)
         {
            error = _T("a role name is lower-case letters, digits, underscore and hyphen");
            return false;
         }
      }

      bool builtin = false;
      BuiltInPermissions_(wanted, builtin);
      if (builtin)
      {
         error = _T("that is a built-in role; its permissions are in the code and cannot be edited");
         return false;
      }

      const unsigned __int64 set = ParsePermissionList_(permissions);
      if (set == 0)
      {
         error = _T("a role needs at least one permission this server has, and may not hold administer_server, administer_domain or manage_roles");
         return false;
      }

      RoleGrant asStored;
      asStored.permissions = set;
      const String normalised = String(DescribeGrant(asStored));

      EnsureLoaded_();

      RoleRecord existing;
      const bool update = FindRole(wanted, existing);

      SQLStatement statement;
      statement.SetTable("hm_roles");

      if (update)
      {
         statement.SetStatementType(SQLStatement::STUpdate);
         String where;
         where.Format(_T("roleid = %I64d"), existing.id);
         statement.SetWhereClause(where);
      }
      else
      {
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("roleid");
         statement.AddColumn("rolename", wanted);
         statement.AddColumnInt64("rolebuiltin", 0);
      }

      statement.AddColumn("roledescription", description);
      statement.AddColumn("rolepermissions", normalised);

      if (!Application::Instance()->GetDBManager()->Execute(statement))
      {
         error = _T("the role could not be saved");
         return false;
      }

      Reload();
      return true;
   }

   bool
   RoleManager::DeleteCustomRole(const String &name, String &error)
   {
      error = _T("");

      RoleRecord record;
      if (!FindRole(name, record))
      {
         error = _T("no such role");
         return false;
      }

      if (record.builtin)
      {
         error = _T("a built-in role cannot be deleted");
         return false;
      }

      SQLCommand removeAssignments("delete from hm_roleassignments where raroleid = @ID");
      removeAssignments.AddParameter("@ID", record.id);
      if (!Application::Instance()->GetDBManager()->Execute(removeAssignments))
      {
         error = _T("the role's assignments could not be removed");
         return false;
      }

      SQLCommand remove("delete from hm_roles where roleid = @ID");
      remove.AddParameter("@ID", record.id);
      if (!Application::Instance()->GetDBManager()->Execute(remove))
      {
         error = _T("the role could not be deleted");
         return false;
      }

      Reload();
      return true;
   }

   std::vector<RoleAssignment>
   RoleManager::ListAssignments()
   {
      EnsureLoaded_();

      boost::lock_guard<boost::recursive_mutex> guard(load_mutex);

      std::vector<RoleAssignment> result;
      for (const auto &entry : assignment_by_account)
         result.push_back(entry.second);

      return result;
   }

   bool
   RoleManager::AssignmentForAccount(__int64 accountId, RoleAssignment &assignment)
   {
      EnsureLoaded_();

      boost::lock_guard<boost::recursive_mutex> guard(load_mutex);

      auto found = assignment_by_account.find(accountId);
      if (found == assignment_by_account.end())
         return false;

      assignment = found->second;
      return true;
   }

   bool
   RoleManager::Assign(__int64 accountId, const String &roleName, const String &domains, String &error)
   {
      error = _T("");

      if (accountId <= 0)
      {
         error = _T("no such account");
         return false;
      }

      if (roleName.IsEmpty())
         return Unassign(accountId, error);

      RoleRecord record;
      if (!FindRole(roleName, record))
      {
         error = _T("no such role");
         return false;
      }

      // Normalised before it is stored, so that a list typed with spaces or in
      // capitals compares equal to the domain names the routes carry.
      const String normalisedDomains = StringParser::JoinVector(SplitDomains_(domains), _T(","));

      RoleAssignment existing;
      const bool update = AssignmentForAccount(accountId, existing);

      SQLStatement statement;
      statement.SetTable("hm_roleassignments");

      if (update)
      {
         statement.SetStatementType(SQLStatement::STUpdate);
         String where;
         where.Format(_T("raid = %I64d"), existing.id);
         statement.SetWhereClause(where);
      }
      else
      {
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("raid");
         statement.AddColumnInt64("raaccountid", accountId);
      }

      statement.AddColumnInt64("raroleid", record.id);
      statement.AddColumn("radomains", normalisedDomains);

      if (!Application::Instance()->GetDBManager()->Execute(statement))
      {
         error = _T("the assignment could not be saved");
         return false;
      }

      Reload();
      return true;
   }

   bool
   RoleManager::Unassign(__int64 accountId, String &error)
   {
      error = _T("");

      SQLCommand remove("delete from hm_roleassignments where raaccountid = @ID");
      remove.AddParameter("@ID", accountId);

      if (!Application::Instance()->GetDBManager()->Execute(remove))
      {
         error = _T("the assignment could not be removed");
         return false;
      }

      Reload();
      return true;
   }

   void
   RoleManager::SetCurrentActor(const RoleActor &actor)
   {
      current_actor = actor;
   }

   void
   RoleManager::ClearCurrentActor()
   {
      current_actor = RoleActor();
   }

   RoleActor
   RoleManager::CurrentActor()
   {
      return current_actor;
   }
}
