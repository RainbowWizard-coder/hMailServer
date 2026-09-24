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
#include "../Cache/Cache.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DALRecordset.h"
#include "../Util/AuditTrail.h"
#include "../Util/Encoding/Base64.h"
#include "../Util/Parsing/StringParser.h"
#include "../Util/Unicode.h"

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
         "manage_roles",
         "send_through_api",
         "provision"
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

      // The custom roles in a backup archive. Named once so that the writer and
      // the reader cannot disagree.
      //
      //   <Roles>
      //     <Role Name="mailops" Description="Runs the queue" Permissions="view_queue,manage_queue"/>
      //   </Roles>
      const wchar_t *const RolesElement = L"Roles";
      const wchar_t *const RoleElement = L"Role";
      const wchar_t *const RoleNameAttribute = L"Name";
      const wchar_t *const RoleDescriptionAttribute = L"Description";
      const wchar_t *const RolePermissionsAttribute = L"Permissions";

      // A description is whatever an administrator typed, and XML 1.0 has no
      // representation at all for most control characters: one of them in an
      // attribute makes the whole archive index unreadable, and every account
      // after it with it. Such a value crosses as base64 of its UTF-8 under the
      // attribute's name with ".base64" appended, which is the convention
      // AccountStores uses for the per-account rows; the reader takes either.
      const wchar_t *const EncodedSuffix = L".base64";

      bool NeedsEncoding(const String &value)
      {
         for (int index = 0; index < value.GetLength(); index++)
         {
            const wchar_t character = value.GetAt(index);

            if (character < 0x20 || character == 0x7f)
               return true;
         }

         return false;
      }

      void WriteRoleAttribute(XNode *node, const String &attribute, const String &value)
      {
         if (!NeedsEncoding(value))
         {
            node->AppendAttr(attribute.c_str(), value.c_str());
            return;
         }

         AnsiString utf8;
         Unicode::WideToMultiByte(value, utf8);

         const String encoded = Base64::Encode(utf8.c_str(), (int) utf8.GetLength());
         const String name = attribute + EncodedSuffix;

         node->AppendAttr(name.c_str(), encoded.c_str());
      }

      String ReadRoleAttribute(XNode *node, const String &attribute)
      {
         LPXAttr plain = node->GetAttr(attribute.c_str());

         if (plain)
            return plain->value;

         const String name = attribute + EncodedSuffix;
         LPXAttr encoded = node->GetAttr(name.c_str());

         if (!encoded)
            return String();

         // Base64 is ASCII, so narrowing it is exact.
         AnsiString ascii;
         for (int index = 0; index < encoded->value.GetLength(); index++)
            ascii += (char) encoded->value.GetAt(index);

         const AnsiString bytes = Base64::Decode(ascii.c_str(), (int) ascii.GetLength());

         String value;
         Unicode::MultiByteToWide(bytes, value);
         return value;
      }

      // THE AUDIT TRAIL'S WORDS for a change to the two role tables. A row of
      // hm_roleassignments is an account id, a role id and a list; recorded as
      // its columns it reads "raaccountid=12, raroleid=3", which says nothing
      // anybody can use, and what it held before an update or a delete is gone
      // by the time the statement has run. So each write below says, through an
      // AuditDescription, the account's address and the role's name as they were
      // and as they are.

      // The account an assignment is for, by the address a person knows it by.
      String AccountAddress(__int64 accountId)
      {
         SQLCommand command("select accountaddress from hm_accounts where accountid = @ACCOUNTID");
         command.AddParameter("@ACCOUNTID", accountId);

         std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);

         if (!recordset || recordset->IsEOF())
            return String(_T("account id ")) + StringParser::IntToString(accountId);

         return recordset->GetStringValue("accountaddress");
      }

      // What an assignment reaches, in the words the listings use.
      String DescribeReach(const String &roleName, const String &domains)
      {
         if (RoleManager::AssignmentReachesEveryDomain(roleName, domains))
            return _T("every domain");

         if (domains.IsEmpty())
            return _T("the account's own domain");

         return domains;
      }

      String DescribeText(const String &text)
      {
         return text.IsEmpty() ? String(_T("(none)")) : String(_T("'")) + text + String(_T("'"));
      }

      // Every level by name, and a number no level has by its number: a
      // hand-edited row or archive can carry one, and a message that called
      // it "domain administrator" because it was not the other would be wrong.
      String AdminLevelName(Account::AdminLevel level)
      {
         switch (level)
         {
         case Account::ServerAdmin:
            return _T("server administrator");
         case Account::DomainAdmin:
            return _T("domain administrator");
         case Account::NormalUser:
            return _T("ordinary account");
         default:
            return String(_T("admin level ")) + StringParser::IntToString((int) level);
         }
      }
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
   RoleManager::ReachesEveryDomain(const RoleGrant &grant)
   {
      return grant.administrative &&
             (grant.Holds(RolePermission::AdministerServer) || grant.domains.empty());
   }

   bool
   RoleManager::IsAdministratorGrant(const RoleGrant &grant)
   {
      return grant.administrative &&
             (grant.Holds(RolePermission::AdministerServer) || grant.Holds(RolePermission::AdministerDomain));
   }

   bool
   RoleManager::AssignmentReachesEveryDomain(const String &roleName, const String &domains)
   {
      String list = domains;
      list.Trim();

      if (list.Compare(_T("*")) == 0)
         return true;

      return list.IsEmpty() && ReachesEveryDomainWhenUnlisted(roleName);
   }

   RoleGrant
   RoleManager::ConfineAssignment_(RoleGrant grant, const String &roleName, const String &domains, const String &ownDomain)
   {
      if (!grant.administrative)
         return grant;

      if (AssignmentReachesEveryDomain(roleName, domains))
      {
         grant.domains.clear();
         return grant;
      }

      if (grant.domains.empty())
         grant.domains.push_back(ownDomain);

      return grant;
   }

   bool
   RoleManager::ReachesEveryDomainWhenUnlisted(const String &roleName)
   {
      const String name = Lower(roleName);
      return name.Compare(_T("serveradmin")) == 0 || name.Compare(_T("auditor")) == 0;
   }

   bool
   RoleManager::Encompasses(const RoleGrant &holder, const RoleGrant &other)
   {
      if (!other.administrative)
         return true;

      if (!holder.administrative)
         return false;

      // The server administrator takes in everything - but only the one that
      // reaches every domain. A grant that holds AdministerServer and carries a
      // list of domains - a Scope=full key issued for named domains, which is
      // serveradmin confined to them, or serveradmin assigned with a list - is
      // confined to that list at every route it reaches, and the rule below
      // holds for it as for any other confined grant. Without this a key issued
      // for a.com could set the password of an a.com account that administers
      // b.com, or that audits every domain, and sign in as it.
      if (holder.Holds(RolePermission::AdministerServer) && holder.domains.empty())
         return true;

      if ((other.permissions & ~holder.permissions) != 0)
         return false;

      // Reaching every domain takes in any list; a list takes in only a list
      // that is inside it.
      if (holder.domains.empty())
         return true;

      // What reaches every domain is never inside a list: a grant with no list
      // of its own, and one that holds AdministerServer whatever list it
      // carries, since Allows and ReachesEveryDomain read that permission as
      // the whole server.
      if (ReachesEveryDomain(other))
         return false;

      for (const String &domain : other.domains)
      {
         if (!IsDomainReached_(holder, domain))
            return false;
      }

      return true;
   }

   RoleGrant
   RoleManager::GrantForAssignment(std::shared_ptr<const Account> account, const String &roleName, const String &domains)
   {
      if (!account)
         return RoleGrant();

      return ConfineAssignment_(GrantForRole(roleName, SplitDomains_(domains)), roleName, domains,
                                Lower(StringParser::ExtractDomain(account->GetAddress())));
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

         // A confined role with no list of its own reaches the account's own
         // domain, and only that. serveradmin and auditor are the exceptions:
         // the one has never been a domain's administrator, and an auditor
         // confined to the domain its own mailbox happens to sit in could not
         // read the settings, the logs or the queue - which are server-wide -
         // and would not be reading everything. And "*" is every domain, for
         // any role, because an administrator said so.
         RoleGrant assignedGrant = ConfineAssignment_(GrantForRole(assignment.role_name, domains),
                                                      assignment.role_name, assignment.domains, ownDomain);

         // The assignment decides, even when the role it names has come to hold
         // nothing: a row edited by hand, or a permission a later version renamed,
         // which ParsePermissionList_ drops. Such an account is an ordinary
         // account and never falls back to its admin level. An account whose
         // level says server administrator, narrowed to a role, must not be the
         // server administrator again because the role lost its permissions, any
         // more than because the role was deleted - which DeleteCustomRole
         // refuses while anybody holds it. Neither editor saves a role whose
         // permissions come to nothing, so an administrator (the password, or an
         // account holding manage_roles) is always there to give it one that
         // holds something, or to take the assignment away.
         return assignedGrant;
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
      bool refused = false;
      return SaveCustomRole_(name, description, permissions, error, refused);
   }

   bool
   RoleManager::SaveCustomRole_(const String &name, const String &description, const String &permissions, String &error, bool &refused)
   {
      error = _T("");

      // Every return below until the write is a refusal of the role itself.
      refused = true;

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

      refused = false;

      // The role by its name, and its permissions and description as they were
      // and as they now are. An update's statement names the row by its id.
      const String detail = update
         ? String(_T("permissions: ")) + existing.permissions + _T(" -> ") + normalised +
           _T("; description: ") + DescribeText(existing.description) + _T(" -> ") + DescribeText(description)
         : String(_T("permissions: ")) + normalised + _T("; description: ") + DescribeText(description);

      AuditDescription described(_T("hm_roles"), wanted, detail);

      if (!Application::Instance()->GetDBManager()->Execute(statement))
      {
         error = _T("the role could not be saved");
         return false;
      }

      Reload();
      return true;
   }

   bool
   RoleManager::CountHolders_(const RoleRecord &record, int &holders)
   {
      holders = 0;

      // Counted from the table and not the copy, so that an assignment the copy
      // has not seen yet still counts. A plain count, which reads the same on
      // all four databases.
      SQLCommand countHolders("select count(*) as c from hm_roleassignments where raroleid = @ROLEID");
      countHolders.AddParameter("@ROLEID", record.id);

      std::shared_ptr<DALRecordset> counted = Application::Instance()->GetDBManager()->OpenRecordset(countHolders);

      if (!counted || counted->IsEOF())
         return false;

      holders = (int) counted->GetLongValue("c");
      return true;
   }

   String
   RoleManager::HeldRoleRefusal_(const RoleRecord &record, int holders)
   {
      return String(_T("The role ")) + record.name + _T(" is held by ") + StringParser::IntToString(holders) +
         (holders == 1 ? _T(" account") : _T(" accounts")) +
         _T(", which must be given another role or have this one taken away before the role can be deleted.");
   }

   bool
   RoleManager::DeleteCustomRole(const String &name, String &error, int &holders)
   {
      error = _T("");
      holders = 0;

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

      // A role somebody holds is not deleted, and its assignments are not taken
      // away to make it deletable. An account with no assignment holds what its
      // admin level gives it, so deleting a held role would hand every holder
      // that authority back with nothing said - and an account whose level says
      // server administrator, narrowed to this role, would be the server
      // administrator again. Each holder is given another role or has this one
      // taken away first: a decision about one account, made by somebody, and
      // on the audit trail as that.
      if (!CountHolders_(record, holders))
      {
         error = _T("the role's assignments could not be read");
         return false;
      }

      if (holders > 0)
      {
         error = HeldRoleRefusal_(record, holders);
         return false;
      }

      // Only a role nobody holds reaches the delete, so the one row it puts on
      // the audit trail records a deletion that happened: every refusal above
      // returned before a statement ran.
      bool deleted = false;
      {
         AuditDescription described(_T("hm_roles"), record.name,
            String(_T("permissions: ")) + record.permissions + _T("; description: ") + DescribeText(record.description));

         SQLCommand remove("delete from hm_roles where roleid = @ID");
         remove.AddParameter("@ID", record.id);
         deleted = Application::Instance()->GetDBManager()->Execute(remove);
      }

      if (!deleted)
      {
         // An assignment written between the count and the delete does not slip
         // through. The assignments' foreign key to hm_roles has no cascade, so
         // the delete fails, the role stays, and the trail - which records only a
         // statement that succeeded - says nothing. Counted again, so that the
         // request that lost that race is told what one that did not lose it is
         // told - who holds the role, and what is to be done first - rather than
         // that the database refused.
         int since = 0;
         if (CountHolders_(record, since) && since > 0)
         {
            holders = since;
            error = HeldRoleRefusal_(record, holders);
            return false;
         }

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

      // The account by its address and the role by its name, as they were and
      // as they now are: the statement itself carries two ids and a list.
      const String detail = update
         ? String(_T("role: ")) + existing.role_name + _T(" -> ") + record.name +
           _T("; domains: ") + DescribeReach(existing.role_name, existing.domains) + _T(" -> ") + DescribeReach(record.name, normalisedDomains)
         : String(_T("role: ")) + record.name + _T("; domains: ") + DescribeReach(record.name, normalisedDomains);

      AuditDescription described(_T("hm_roleassignments"), AccountAddress(accountId), detail);

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

      // Nothing held is nothing to take away: no statement, and so no row on
      // the audit trail saying "(none) -> (none)". COM's AssignRole with an
      // empty role name reaches here for any account; the REST route asks
      // first and answers 404.
      RoleAssignment existing;
      if (!AssignmentForAccount(accountId, existing))
         return true;

      const String detail = String(_T("role: ")) + existing.role_name + _T(" -> (none); domains: ") +
                            DescribeReach(existing.role_name, existing.domains) + _T(" -> (none)");

      AuditDescription described(_T("hm_roleassignments"), AccountAddress(accountId), detail);

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

   bool
   RoleManager::XMLStore(XNode *backupNode)
   {
      // The table, not the copy RoleManager decides from: a role whose
      // permission column holds a token this build does not know must still
      // cross the archive as it is, and the restore judges it again.
      SQLCommand selectRoles("select rolename, roledescription, rolebuiltin, rolepermissions from hm_roles");
      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(selectRoles);

      if (!recordset)
      {
         Logger::Instance()->LogBackup("The custom roles could not be read from hm_roles, so they cannot be carried in this backup.");
         return false;
      }

      XNode *rolesNode = backupNode->AppendChild(RolesElement);

      while (!recordset->IsEOF())
      {
         const String roleName = recordset->GetStringValue("rolename");

         // A built-in role is on every server, with its permissions in the code
         // rather than in this row. A custom row that has a built-in name - which
         // only a hand edit makes - is not carried either: no restore could put
         // it back.
         bool namedLikeBuiltIn = false;
         BuiltInPermissions_(roleName, namedLikeBuiltIn);

         if (recordset->GetLongValue("rolebuiltin") == 0 && !namedLikeBuiltIn && !roleName.IsEmpty())
         {
            XNode *roleNode = rolesNode->AppendChild(RoleElement);
            WriteRoleAttribute(roleNode, RoleNameAttribute, roleName);
            WriteRoleAttribute(roleNode, RoleDescriptionAttribute, recordset->GetStringValue("roledescription"));
            WriteRoleAttribute(roleNode, RolePermissionsAttribute, recordset->GetStringValue("rolepermissions"));
         }

         recordset->MoveNext();
      }

      // A server with no custom role writes the archive it wrote before this
      // existed, which keeps two backups of it comparable across the change.
      if (rolesNode->GetChildCount() == 0)
         backupNode->RemoveChild(rolesNode);

      return true;
   }

   bool
   RoleManager::XMLLoad(XNode *backupNode)
   {
      XNode *rolesNode = backupNode->GetChild(RolesElement);

      // An archive taken before custom roles were carried, or from a server that
      // had none. Nothing to put back - and nothing to take away: a custom role
      // this server has is not evidence the archive's server lacked it.
      if (!rolesNode)
         return true;

      unsigned int restored = 0;
      unsigned int kept = 0;
      String restoredNames;

      for (int index = 0; index < rolesNode->GetChildCount(); index++)
      {
         XNode *roleNode = rolesNode->GetChild((unsigned int) index);

         if (roleNode->name != RoleElement)
            continue;

         const String roleName = ReadRoleAttribute(roleNode, RoleNameAttribute);
         const String description = ReadRoleAttribute(roleNode, RoleDescriptionAttribute);
         const String permissions = ReadRoleAttribute(roleNode, RolePermissionsAttribute);

         // A role this server already has is KEPT as it is here - see the
         // header. The difference, if there is one, is named with both sides,
         // so that the administrator who wants the archive's definition makes
         // that change and it is recorded as theirs.
         RoleRecord existing;
         if (FindRole(roleName, existing))
         {
            kept++;

            RoleGrant asArchived;
            asArchived.permissions = ParsePermissionList_(permissions);
            const String archivedPermissions = String(DescribeGrant(asArchived));

            if (existing.builtin)
            {
               Logger::Instance()->LogBackup(Formatter::Format(
                  "The backup carries a role named {0}, which is a built-in role here; its permissions are in the code, and the backup's ({1}) have been ignored.",
                  roleName, archivedPermissions));
            }
            else if (existing.permissions != archivedPermissions || existing.description != description)
            {
               Logger::Instance()->LogBackup(Formatter::Format(
                  "The backup carries the custom role {0}, which this server already has. This server's definition has been kept: permissions {1} here and {2} in the backup; description {3} here and {4} in the backup. Every credential holding the role keeps what this server gives it; change the role deliberately if the backup's definition is wanted.",
                  roleName, DescribeText(existing.permissions), DescribeText(archivedPermissions),
                  DescribeText(existing.description), DescribeText(description)));
            }
            else
            {
               Logger::Instance()->LogBackup(Formatter::Format(
                  "The backup carries the custom role {0}, which this server already has with the same permissions and description. Left as it is.",
                  roleName));
            }

            continue;
         }

         String error;
         bool refused = false;

         // Through the one writer a role has, so that a restored role is judged
         // exactly as a role made over REST is: no built-in name, no permission
         // that hands out authority, none this build does not know.
         if (SaveCustomRole_(roleName, description, permissions, error, refused))
         {
            restored++;

            if (!restoredNames.IsEmpty())
               restoredNames += _T(", ");
            restoredNames += roleName;

            continue;
         }

         if (!refused)
         {
            // The roles before this one in the archive are written already; a
            // message that said nothing had changed would be wrong.
            Logger::Instance()->LogBackup(Formatter::Format(
               "The custom role {0} could not be written to hm_roles: {1}. The restore has been stopped: no domain has been removed, and {2} custom role(s) the backup carries had already been created before the failure{3}.",
               roleName, error, restored,
               restoredNames.IsEmpty() ? String() : String(_T(" (")) + restoredNames + _T(")")));
            return false;
         }

         Logger::Instance()->LogBackup(Formatter::Format(
            "The backup carries the custom role {0}, which has not been restored: {1}. An account the backup gives this role is restored without it.",
            roleName, error));
      }

      Logger::Instance()->LogBackup(Formatter::Format(
         "Restored {0} custom role(s). {1} role(s) the backup carries were already on this server and have been kept as they are here.",
         restored, kept));

      return true;
   }

   bool
   RoleManager::WriteAdminLevel_(Account &account, Account::AdminLevel level)
   {
      SQLCommand write("update hm_accounts set accountadminlevel = @LEVEL where accountid = @ACCOUNTID");
      write.AddParameter("@LEVEL", (int) level);
      write.AddParameter("@ACCOUNTID", account.GetID());

      if (!Application::Instance()->GetDBManager()->Execute(write))
         return false;

      account.SetAdminLevel(level);
      Cache<Account>::Instance()->RemoveObject(account.GetAddress());

      return true;
   }

   bool
   RoleManager::RestoreArchivedAdminLevel(Account &account, Account::AdminLevel archivedLevel)
   {
      // An ordinary account's level was never withheld, and a level already in
      // place needs nothing.
      if (archivedLevel == Account::NormalUser || account.GetAdminLevel() == archivedLevel)
         return true;

      if (WriteAdminLevel_(account, archivedLevel))
         return true;

      // Not given back. That is the safe side of this - the account holds its
      // role and nothing more - so the restore goes on, and both logs say what
      // the account has not got.
      const String message = Formatter::Format(
         "The admin level the backup carries for {0} ({1}) could not be written back behind its restored role assignment. {0} holds its role and nothing more; give it the admin level deliberately if it is wanted.",
         account.GetAddress(), AdminLevelName(archivedLevel));

      Logger::Instance()->LogBackup(message);
      ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6130, "RoleManager::RestoreArchivedAdminLevel", message);

      return true;
   }

   bool
   RoleManager::RevokeUnrestoredRole(Account &account, const String &roleName, Account::AdminLevel archivedLevel)
   {
      const String missing = roleName.IsEmpty() ? String(_T("(no role named)")) : roleName;

      // The row is an ordinary account's already when Account::XMLLoad saw the
      // assignment coming, which is every way here; made sure of all the same,
      // because this is the one place the wider level must not stand.
      if (account.GetAdminLevel() != Account::NormalUser && !WriteAdminLevel_(account, Account::NormalUser))
      {
         Logger::Instance()->LogBackup(Formatter::Format(
            "The backup gives {0} the role {1}, which neither the backup nor this server has, and its admin level could not be lowered in the assignment's place. The restore has been stopped rather than leave {0} holding the authority of its admin level.",
            account.GetAddress(), missing));

         return false;
      }

      if (archivedLevel == Account::NormalUser)
      {
         Logger::Instance()->LogBackup(Formatter::Format(
            "The backup gives {0} the role {1}, which neither the backup nor this server has. The assignment has not been restored: {0} is an ordinary account until an administrator gives it a role.",
            account.GetAddress(), missing));

         return true;
      }

      const String message = Formatter::Format(
         "The backup gives {0} the role {1}, which neither the backup nor this server has. The assignment has not been restored, and because the account's admin level ({2}) would otherwise have given it authority the role had narrowed away, its admin level is now that of an ordinary account. Give {0} a role or an admin level to restore its authority.",
         account.GetAddress(), missing, AdminLevelName(archivedLevel));

      Logger::Instance()->LogBackup(message);

      // The error log and the Control Panel as well as the backup log: the
      // restore goes on to report success, and an account that has lost its
      // authority is not a line an administrator should have to go looking for.
      ErrorManager::Instance()->ReportError(ErrorManager::Medium, 6129, "RoleManager::RevokeUnrestoredRole", message);

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
