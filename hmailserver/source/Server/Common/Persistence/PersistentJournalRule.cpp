// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "stdafx.h"
#include "PersistentJournalRule.h"

#include "../BO/JournalRule.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../Cache/CacheContainer.h"
#include "../Util/Journaling.h"
#include "../Util/Parsing/StringParser.h"

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      // A control character anywhere in the value. StringParser's address check
      // is not enough on its own here: its local-part class excludes the angle
      // brackets, the space, the backslash and the quote and nothing else, so a
      // carriage return or a line feed in the local part passes it. A journal
      // address is written into a header of a message this server generates and
      // handed to the parser that builds an outbound RCPT TO, and either of those
      // is a header injection if the value can carry a line break.
      //
      // The tab counts as one here, unlike in the header writer: a tab is white
      // space inside a header field body and belongs in a folded subject, and it
      // has no business inside an email address.
      bool HasControlCharacter(const String &value)
      {
         for (int i = 0; i < value.GetLength(); i++)
         {
            // Through unsigned, so that a wide character a signed wchar_t could
            // hold as a negative number is not read as a control character.
            const unsigned int character = (unsigned int) value.GetAt(i);

            if (character < 0x20 || character == 0x7F)
               return true;
         }

         return false;
      }
   }

   PersistentJournalRule::PersistentJournalRule()
   {

   }

   PersistentJournalRule::~PersistentJournalRule()
   {

   }

   bool
   PersistentJournalRule::DeleteObject(std::shared_ptr<JournalRule> rule)
   {
      if (rule->GetID() == 0)
         return false;

      SQLCommand command("delete from hm_journalrules where jrid = @JRID");
      command.AddParameter("@JRID", rule->GetID());

      const bool deleted = Application::Instance()->GetDBManager()->Execute(command);

      Journaling::SetNeedRefresh();

      return deleted;
   }

   bool
   PersistentJournalRule::DeleteByDomain(__int64 domainId)
   {
      SQLCommand command("delete from hm_journalrules where jrdomainid = @DOMAINID");
      command.AddParameter("@DOMAINID", domainId);

      const bool deleted = Application::Instance()->GetDBManager()->Execute(command);

      Journaling::SetNeedRefresh();

      return deleted;
   }

   bool
   PersistentJournalRule::DeleteByAccount(__int64 accountId)
   {
      SQLCommand command("delete from hm_journalrules where jraccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountId);

      const bool deleted = Application::Instance()->GetDBManager()->Execute(command);

      Journaling::SetNeedRefresh();

      return deleted;
   }

   bool
   PersistentJournalRule::ResolveTarget(std::shared_ptr<JournalRule> rule, String &errorMessage)
   {
      String target = rule->GetTarget();
      target.Trim();
      target.ToLower();
      rule->SetTarget(target);

      if (target.IsEmpty())
      {
         errorMessage = "A journal rule must name a domain or an account address.";
         return false;
      }

      if (rule->GetScope() == JournalRule::ScopeAccount)
      {
         if (target.Find(_T("@")) < 0)
         {
            errorMessage = "An account journal rule must name an account by its address.";
            return false;
         }

         std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(target);
         if (!account)
         {
            errorMessage = "No account with the address " + target + " exists on this server.";
            return false;
         }

         rule->SetAccountID(account->GetID());
         rule->SetDomainID(account->GetDomainID());
         return true;
      }

      if (target.Find(_T("@")) >= 0 || target.Find(_T(" ")) >= 0)
      {
         errorMessage = "A domain journal rule must name a domain this server hosts, not an address.";
         return false;
      }

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(target);
      if (!domain)
      {
         errorMessage = "No domain named " + target + " is hosted on this server.";
         return false;
      }

      rule->SetDomainID(domain->GetID());
      rule->SetAccountID(0);
      return true;
   }

   bool
   PersistentJournalRule::SaveObject(std::shared_ptr<JournalRule> rule)
   {
      String errorMessage;
      return SaveObject(rule, errorMessage, PersistenceModeNormal);
   }

   bool
   PersistentJournalRule::SaveObject(std::shared_ptr<JournalRule> rule, String &errorMessage, PersistenceMode mode)
   {
      if (mode == PersistenceModeRestore)
      {
         // JournalRules::PreSaveObject has given the rule the domain the restore
         // made; an account rule finds its account by the address the archive
         // carries, under that domain's accounts, which are restored before the
         // rules. A rule whose account did not come back is left out of the
         // restore with the reason rather than saved pointing at nothing.
         if (rule->GetScope() == JournalRule::ScopeAccount)
         {
            std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(rule->GetTarget());
            if (!account || account->GetDomainID() != rule->GetDomainID())
            {
               errorMessage = "The account " + rule->GetTarget() + " named by a journal rule was not restored with its domain.";
               return false;
            }

            rule->SetAccountID(account->GetID());
         }
      }
      else if (!ResolveTarget(rule, errorMessage))
      {
         return false;
      }

      // The one thing every surface must agree on, checked here rather than in
      // any of them: a rule with no journal address, or one that is not an
      // address, would be a rule that copies mail to nowhere, and the copy would
      // fail silently once per message for as long as the rule existed. The
      // restore is held to it too - a rule that cannot deliver is not worth
      // bringing back.
      String journalAddress = rule->GetJournalAddress();
      journalAddress.Trim();
      journalAddress.ToLower();
      rule->SetJournalAddress(journalAddress);

      if (journalAddress.IsEmpty())
      {
         errorMessage = "A journal rule must name the address the copy goes to.";
         return false;
      }

      // Checked as an address, by the same function every other address on this
      // server is checked by, rather than by hand. What it used to be - not
      // empty, an @ in it, no space - let a carriage return or a line feed
      // through, and this value is written into a header of a message this server
      // generates and handed to the parser that builds the outbound RCPT TO. A
      // journal address is one of the very few fields an administrator can put
      // there, and no field that reaches a header should be trusted to be one
      // line because nobody has typed two. The function also caps the address at
      // 254 characters, which is what stops a longer one being cut down to the
      // column's 255 and sending the copies somewhere else.
      if (HasControlCharacter(journalAddress))
      {
         // The value is deliberately not repeated back: a sentence carrying a
         // line break is the same trouble one line further on, in the log and in
         // whatever reads it.
         errorMessage = "The journal address contains a control character.";
         return false;
      }

      if (!StringParser::IsValidEmailAddress(journalAddress))
      {
         errorMessage = "The journal address " + journalAddress + " is not an email address.";
         return false;
      }

      SQLStatement statement;

      statement.SetTable("hm_journalrules");

      statement.AddColumn("jrscope", (long) rule->GetScope());
      statement.AddColumnInt64("jrdomainid", rule->GetDomainID());
      statement.AddColumnInt64("jraccountid", rule->GetAccountID());
      statement.AddColumn("jraddress", rule->GetJournalAddress());
      statement.AddColumn("jrdirection", (long) rule->GetDirection());
      statement.AddColumn("jrmode", (long) rule->GetMode());
      statement.AddColumn("jrenabled", rule->GetEnabled() ? 1 : 0);
      statement.AddColumn("jrnote", rule->GetNote());

      bool isNewObject = rule->GetID() == 0;

      if (isNewObject)
      {
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("jrid");
      }
      else
      {
         statement.SetStatementType(SQLStatement::STUpdate);

         String where;
         where.Format(_T("jrid = %I64d"), rule->GetID());
         statement.SetWhereClause(where);
      }

      __int64 databaseID = 0;
      bool result = Application::Instance()->GetDBManager()->Execute(statement, isNewObject ? &databaseID : 0);

      if (result && isNewObject)
         rule->SetID(databaseID);

      // The next accepted message reads the table again.
      Journaling::SetNeedRefresh();

      return result;
   }

   bool
   PersistentJournalRule::ReadObject(std::shared_ptr<JournalRule> rule, __int64 id)
   {
      SQLCommand command("select * from hm_journalrules where jrid = @JRID");

      // The whole id, not the low half of it. AddParameter is overloaded on int,
      // unsigned int and __int64; an __int64 argument names the __int64 overload
      // on every compiler, and an id no row carries now simply finds no row.
      command.AddParameter("@JRID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      if (recordset->IsEOF())
         return false;

      return ReadObject(rule, recordset);
   }

   bool
   PersistentJournalRule::ReadObject(std::shared_ptr<JournalRule> rule, std::shared_ptr<DALRecordset> recordset)
   {
      rule->SetID(recordset->GetInt64Value("jrid"));
      rule->SetScope(recordset->GetLongValue("jrscope") == JournalRule::ScopeAccount ? JournalRule::ScopeAccount : JournalRule::ScopeDomain);
      rule->SetDomainID(recordset->GetInt64Value("jrdomainid"));
      rule->SetAccountID(recordset->GetInt64Value("jraccountid"));
      rule->SetJournalAddress(recordset->GetStringValue("jraddress"));

      // A value the column should not hold lands on "both", the direction that
      // copies the most: an unrecognised number must not quietly halve what a
      // rule covers.
      const long direction = recordset->GetLongValue("jrdirection");
      rule->SetDirection((direction >= JournalRule::DirectionInbound && direction <= JournalRule::DirectionBoth)
         ? (JournalRule::Direction) direction : JournalRule::DirectionBoth);

      rule->SetMode(recordset->GetLongValue("jrmode") == JournalRule::ModeCopy ? JournalRule::ModeCopy : JournalRule::ModeReport);
      rule->SetEnabled(recordset->GetLongValue("jrenabled") ? true : false);
      rule->SetNote(recordset->GetStringValue("jrnote"));

      // The name the ids stand for, so every surface shows what the administrator
      // typed rather than a number. A rule whose account or domain is gone shows
      // an empty target and matches nothing, since the ids name nothing.
      String target;
      if (rule->GetScope() == JournalRule::ScopeAccount)
      {
         std::shared_ptr<const Account> account = CacheContainer::Instance()->GetAccount(rule->GetAccountID());
         if (account)
            target = account->GetAddress();
      }
      else
      {
         std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(rule->GetDomainID());
         if (domain)
            target = domain->GetName();
      }

      rule->SetTarget(target);

      return true;
   }
}
