// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"
#include "MaskedAddresses.h"
#include "Time.h"
#include "Parsing/StringParser.h"
#include "../Application/Application.h"
#include "../BO/Account.h"
#include "../Cache/CacheContainer.h"
#include "../SQL/DALRecordset.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"

#include <openssl/rand.h>
#include <stdexcept>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{
   namespace
   {
      const char *const Columns = "maid, maaccountid, maaddress, malabel, maactive, mareceived, macreated";

      MaskedAddresses::Entry ReadEntry(std::shared_ptr<DALRecordset> recordset)
      {
         MaskedAddresses::Entry entry;
         entry.id = recordset->GetInt64Value("maid");
         entry.account_id = recordset->GetInt64Value("maaccountid");
         entry.address = recordset->GetStringValue("maaddress");
         entry.label = recordset->GetStringValue("malabel");
         entry.active = recordset->GetLongValue("maactive") == 1;
         entry.received = (int) recordset->GetLongValue("mareceived");
         entry.created = recordset->GetStringValue("macreated");
         return entry;
      }

      // Six characters from an alphabet without the ones people misread.
      String RandomTail()
      {
         static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
         unsigned char bytes[6];
         if (RAND_bytes(bytes, sizeof(bytes)) != 1)
            return String();

         String tail;
         for (size_t i = 0; i < sizeof(bytes); i++)
            tail += (wchar_t) alphabet[bytes[i] % (sizeof(alphabet) - 1)];
         return tail;
      }

      bool Taken(const String &address)
      {
         if (CacheContainer::Instance()->GetAccount(address) || CacheContainer::Instance()->GetAlias(address) ||
             CacheContainer::Instance()->GetDistributionList(address))
            return true;
         MaskedAddresses::Entry existing;
         return MaskedAddresses::FindByAddress(address, existing);
      }

      void Expect(bool condition, const char *message)
      {
         if (!condition)
            throw std::logic_error(message);
      }
   }

   String
   MaskedAddresses::Slug(const String &label)
   {
      String slug;
      for (int i = 0; i < label.GetLength() && slug.GetLength() < 20; i++)
      {
         wchar_t c = label[i];
         if (c >= L'A' && c <= L'Z')
            c = (wchar_t) (c - L'A' + L'a');
         if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9'))
            slug += c;
      }
      return slug.IsEmpty() ? String(_T("masked")) : slug;
   }

   bool
   MaskedAddresses::List(__int64 accountId, std::vector<Entry> &entries)
   {
      entries.clear();
      SQLCommand command(AnsiString("select ") + Columns + " from hm_maskedaddresses where maaccountid = @ACCOUNTID order by maid desc");
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;
      while (!recordset->IsEOF())
      {
         entries.push_back(ReadEntry(recordset));
         recordset->MoveNext();
      }
      return true;
   }

   bool
   MaskedAddresses::Find(__int64 accountId, __int64 id, Entry &entry)
   {
      SQLCommand command(AnsiString("select ") + Columns + " from hm_maskedaddresses where maaccountid = @ACCOUNTID and maid = @ID");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@ID", id);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;
      entry = ReadEntry(recordset);
      return true;
   }

   int
   MaskedAddresses::Count(__int64 accountId)
   {
      SQLCommand command("select count(*) as c from hm_maskedaddresses where maaccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return 0;
      return (int) recordset->GetLongValue("c");
   }

   bool
   MaskedAddresses::FindByAddress(const String &address, Entry &entry)
   {
      // SQL Compact refuses a comparison with a parameter longer than the column.
      String wanted = address;
      wanted.Trim();
      wanted.ToLower();
      if (wanted.IsEmpty() || wanted.GetLength() > 255 || wanted.Find(_T("@")) <= 0)
         return false;

      SQLCommand command(AnsiString("select ") + Columns + " from hm_maskedaddresses where maaddress = @ADDRESS");
      command.AddParameter("@ADDRESS", wanted);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return false;
      entry = ReadEntry(recordset);
      return true;
   }

   bool
   MaskedAddresses::Create(std::shared_ptr<const Account> account, const String &label, Entry &entry, String &problem)
   {
      const String domain = StringParser::ExtractDomain(account->GetAddress());
      const String slug = Slug(label);

      String address;
      for (int attempt = 0; attempt < 5 && address.IsEmpty(); attempt++)
      {
         const String tail = RandomTail();
         if (tail.IsEmpty())
            break;
         String candidate = slug + _T(".") + tail + _T("@") + domain;
         candidate.ToLower();
         if (!Taken(candidate))
            address = candidate;
      }

      if (address.IsEmpty())
      {
         problem = _T("no free address could be made; try again");
         return false;
      }

      SQLStatement statement(SQLStatement::STInsert, "hm_maskedaddresses");
      statement.AddColumnInt64("maaccountid", account->GetID());
      statement.AddColumn("maaddress", address);
      statement.AddColumn("malabel", label);
      statement.AddColumn("maactive", 1);
      statement.AddColumn("mareceived", 0);
      statement.AddColumn("macreated", Time::GetCurrentDateTime());
      statement.SetIdentityColumn("maid");

      __int64 made = 0;
      if (!Application::Instance()->GetDBManager()->Execute(statement, &made) || made <= 0)
      {
         problem = _T("the address could not be saved");
         return false;
      }

      return Find(account->GetID(), made, entry);
   }

   bool
   MaskedAddresses::Update(__int64 accountId, __int64 id, const String &label, bool active)
   {
      SQLCommand command("update hm_maskedaddresses set malabel = @LABEL, maactive = @ACTIVE where maaccountid = @ACCOUNTID and maid = @ID");
      command.AddParameter("@LABEL", label);
      command.AddParameter("@ACTIVE", active ? 1 : 0);
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@ID", id);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   MaskedAddresses::Delete(__int64 accountId, __int64 id)
   {
      SQLCommand command("delete from hm_maskedaddresses where maaccountid = @ACCOUNTID and maid = @ID");
      command.AddParameter("@ACCOUNTID", accountId);
      command.AddParameter("@ID", id);
      return Application::Instance()->GetDBManager()->Execute(command);
   }

   void
   MaskedAddresses::CountDelivery(__int64 id)
   {
      SQLCommand command("update hm_maskedaddresses set mareceived = mareceived + 1 where maid = @ID");
      command.AddParameter("@ID", id);
      Application::Instance()->GetDBManager()->Execute(command);
   }

   void
   MaskedAddressesTester::Test()
   {
      Expect(MaskedAddresses::Slug(_T("The Coffee Shop!")) == _T("thecoffeeshop"), "A label keeps its letters and digits, lower-cased.");
      Expect(MaskedAddresses::Slug(_T("A very long name for one online shop")) == _T("averylongnameforoneo"), "A slug is at most 20 characters.");
      Expect(MaskedAddresses::Slug(_T("  --  ")) == _T("masked"), "A label with no letters gives 'masked'.");
      Expect(MaskedAddresses::Slug(L"Caf\u00e9 24") == _T("caf24"), "A letter outside a-z is left out.");
   }
}
