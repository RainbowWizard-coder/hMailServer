// https://www.progressiverobot.com
// Copyright (c) 2026 Christopher Holloway / Progressive Robot Ltd
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "StdAfx.h"

#include "OpenPgpKeyStore.h"
#include "HttpsClient.h"
#include "OpenPgpKey.h"

#include "Unicode.h"
#include "Encoding/Base64.h"
#include "../Application/Application.h"
#include "../Application/Configuration.h"
#include "../BO/Account.h"
#include "../BO/Domain.h"
#include "../Cache/CacheContainer.h"
#include "../SQL/SQLCommand.h"
#include "../SQL/SQLStatement.h"
#include "../SQL/DALRecordset.h"

#include <ctime>
#include <set>
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
      const char *Columns = "pgpkid, pgpkaccountid, pgpkfingerprint, pgpkversion, pgpkalgorithm, pgpkbits, "
                            "pgpkuserid, pgpkcreated, pgpkexpires, pgpkbytes, pgpkkey, pgpkuploaded, pgpkuploadedby";

      __int64 Now()
      {
         return (__int64) time(nullptr);
      }

      AnsiString Utf8(const String &value)
      {
         AnsiString out;
         Unicode::WideToMultiByte(value, out);
         return out;
      }

      String Wide(const AnsiString &value)
      {
         String out;
         Unicode::MultiByteToWide(value, out);
         return out;
      }

      AnsiString Escape(const AnsiString &value)
      {
         AnsiString out;
         for (int i = 0; i < value.GetLength(); i++)
         {
            unsigned char c = (unsigned char) value[i];
            if (c == '"') out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else if (c < 0x20) { char buffer[8]; sprintf_s(buffer, sizeof(buffer), "\\u%04x", c); out += buffer; }
            else out += (char) c;
         }
         return out;
      }

      void ReadRow(std::shared_ptr<DALRecordset> recordset, OpenPgpKeyStore::Row &row)
      {
         row.id = recordset->GetInt64Value("pgpkid");
         row.account_id = recordset->GetInt64Value("pgpkaccountid");
         row.fingerprint = AnsiString(recordset->GetStringValue("pgpkfingerprint"));
         row.version = (int) recordset->GetLongValue("pgpkversion");
         row.algorithm = (int) recordset->GetLongValue("pgpkalgorithm");
         row.bits = (int) recordset->GetLongValue("pgpkbits");
         row.user_id = recordset->GetStringValue("pgpkuserid");
         row.created = recordset->GetInt64Value("pgpkcreated");
         row.expires = recordset->GetInt64Value("pgpkexpires");
         row.size = (int) recordset->GetLongValue("pgpkbytes");
         row.key_base64 = AnsiString(recordset->GetStringValue("pgpkkey"));
         row.uploaded = recordset->GetInt64Value("pgpkuploaded");
         row.uploaded_by = recordset->GetStringValue("pgpkuploadedby");
      }

      String LocalPartOf(const String &address)
      {
         int at = address.Find(_T("@"));
         return at > 0 ? address.Mid(0, at) : String();
      }

      String DomainOf(const String &address)
      {
         int at = address.Find(_T("@"));
         return at >= 0 ? address.Mid(at + 1) : String();
      }
   }

   bool
   OpenPgpKeyStore::List(__int64 accountId, std::vector<Row> &rows)
   {
      rows.clear();

      SQLCommand command(String(_T("select ")) + String(Columns) + _T(" from hm_pgpkeys where pgpkaccountid = @ACCOUNTID order by pgpkid asc"));
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         Row row;
         ReadRow(recordset, row);
         rows.push_back(row);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   OpenPgpKeyStore::ListDomain(std::shared_ptr<const Domain> domain, std::vector<DomainKey> &keys)
   {
      keys.clear();

      if (!domain)
         return false;

      SQLCommand command(String(_T("select a.accountaddress, a.accountopenpgppublish, ")) + String(Columns) +
                         _T(" from hm_pgpkeys inner join hm_accounts a on a.accountid = hm_pgpkeys.pgpkaccountid ")
                         _T("where a.accountdomainid = @DOMAINID order by a.accountaddress asc, hm_pgpkeys.pgpkid asc"));
      command.AddParameter("@DOMAINID", domain->GetID());

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      while (!recordset->IsEOF())
      {
         DomainKey key;
         key.address = recordset->GetStringValue("accountaddress");
         key.published = recordset->GetLongValue("accountopenpgppublish") == 1;
         ReadRow(recordset, key.row);
         keys.push_back(key);
         recordset->MoveNext();
      }

      return true;
   }

   bool
   OpenPgpKeyStore::Find(__int64 accountId, const AnsiString &fingerprint, Row &row)
   {
      // Matched in code rather than in the WHERE clause. A fingerprint is short,
      // an account holds at most OpenPGPKeysPerAccount of them, and the one
      // comparison this store makes against text a stranger typed is then a C++
      // one - no dialect's idea of collation, and no long text parameter for SQL
      // Server Compact to be unhappy about.
      AnsiString wanted = fingerprint;
      wanted.MakeLower();

      std::vector<Row> rows;
      if (!List(accountId, rows))
         return false;

      for (size_t i = 0; i < rows.size(); i++)
      {
         if (rows[i].fingerprint == wanted)
         {
            row = rows[i];
            return true;
         }
      }

      return false;
   }

   int
   OpenPgpKeyStore::Count(__int64 accountId)
   {
      SQLCommand command("select count(*) as c from hm_pgpkeys where pgpkaccountid = @ACCOUNTID");
      command.AddParameter("@ACCOUNTID", accountId);

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset || recordset->IsEOF())
         return 0;

      return (int) recordset->GetLongValue("c");
   }

   bool
   OpenPgpKeyStore::Store(std::shared_ptr<const Account> account, const AnsiString &keyText,
                          bool allowAddressMismatch, const String &uploadedBy,
                          Row &row, bool &replaced, AnsiString &problem)
   {
      replaced = false;

      if (!account)
      {
         problem = "There is no such account.";
         return false;
      }

      const int maximumBytes = Configuration::Instance()->GetOpenPGPKeyMaximumBytes();
      const int maximumKeys = Configuration::Instance()->GetOpenPGPKeysPerAccount();

      OpenPgpKey::Parsed key = OpenPgpKey::ParseText(keyText, (size_t) maximumBytes);
      if (!key.ok)
      {
         problem = key.error;
         return false;
      }

      // The key has to be the account's own, unless an administrator says this
      // one is not. A key published under an address whose owner it does not name
      // is a key every correspondent will encrypt to and its owner cannot read.
      if (Configuration::Instance()->GetOpenPGPRequireAddressInUserId() && !allowAddressMismatch)
      {
         AnsiString address = Utf8(account->GetAddress());
         if (!OpenPgpKey::CarriesAddress(key, address))
         {
            problem.Format("None of this key's user ids carries the address %hs. Add that address to the key as a user id, or have an administrator upload it with the address check turned off for this one upload.",
                           Escape(address).c_str());
            return false;
         }
      }

      Row existing;
      bool present = Find(account->GetID(), key.fingerprint, existing);
      if (!present && Count(account->GetID()) >= maximumKeys)
      {
         problem.Format("This account already holds %d OpenPGP keys, which is the most the server allows. Remove one first.", maximumKeys);
         return false;
      }

      AnsiString base64 = Base64::Encode((const char *) &key.bytes[0], (int) key.bytes.size());

      SQLStatement statement;
      statement.SetTable("hm_pgpkeys");
      if (present)
      {
         statement.SetStatementType(SQLStatement::STUpdate);
         String where;
         where.Format(_T("pgpkid = %I64d"), existing.id);
         statement.SetWhereClause(where);
      }
      else
      {
         statement.SetStatementType(SQLStatement::STInsert);
         statement.SetIdentityColumn("pgpkid");
         statement.AddColumnInt64("pgpkaccountid", account->GetID());
         statement.AddColumn("pgpkfingerprint", String(key.fingerprint));
      }

      statement.AddColumn("pgpkversion", (long) key.version);
      statement.AddColumn("pgpkalgorithm", (long) key.algorithm);
      statement.AddColumn("pgpkbits", (long) key.bits);
      statement.AddColumn("pgpkuserid", Wide(key.primary_user_id).Mid(0, 255));
      statement.AddColumnInt64("pgpkcreated", key.created);
      statement.AddColumnInt64("pgpkexpires", key.expires);
      statement.AddColumn("pgpkbytes", (long) key.size);
      statement.AddColumn("pgpkkey", String(base64));
      statement.AddColumnInt64("pgpkuploaded", Now());
      statement.AddColumn("pgpkuploadedby", uploadedBy.Mid(0, 255));

      __int64 id = existing.id;
      if (present)
      {
         if (!Application::Instance()->GetDBManager()->Execute(statement))
         {
            problem = "The key could not be stored.";
            return false;
         }
      }
      else if (!Application::Instance()->GetDBManager()->Execute(statement, &id) || id == 0)
      {
         problem = "The key could not be stored.";
         return false;
      }

      if (!Find(account->GetID(), key.fingerprint, row))
      {
         problem = "The key was stored and could not be read back.";
         return false;
      }

      replaced = present;
      return true;
   }

   bool
   OpenPgpKeyStore::Remove(__int64 accountId, const AnsiString &fingerprint)
   {
      Row row;
      if (!Find(accountId, fingerprint, row))
         return false;

      SQLCommand command("delete from hm_pgpkeys where pgpkid = @ID");
      command.AddParameter("@ID", row.id);

      return Application::Instance()->GetDBManager()->Execute(command);
   }

   bool
   OpenPgpKeyStore::PublishingAllowed(std::shared_ptr<const Domain> domain)
   {
      if (!domain)
         return false;
      if (!Configuration::Instance()->GetOpenPGPWebKeyDirectoryEnabled())
         return false;
      if (!domain->GetIsActive())
         return false;

      return domain->GetOpenPGPPublish();
   }

   bool
   OpenPgpKeyStore::Lookup(std::shared_ptr<const Domain> domain, const AnsiString &hashedLocalPart, AnsiString &keyBytes)
   {
      keyBytes = "";

      if (!PublishingAllowed(domain))
         return false;

      AnsiString wanted = hashedLocalPart;
      wanted.MakeLower();
      if (wanted.GetLength() != 32)
         return false;

      // Only the accounts of this domain that are published AND hold a key, which
      // is usually none of them and never many. The hash cannot be inverted, so
      // the local parts come back and are hashed here; nothing stored has to be
      // kept in step with an account's address.
      SQLCommand command("select distinct a.accountid, a.accountaddress from hm_accounts a "
                         "inner join hm_pgpkeys k on k.pgpkaccountid = a.accountid "
                         "where a.accountdomainid = @DOMAINID and a.accountactive = 1 and a.accountopenpgppublish = 1");
      command.AddParameter("@DOMAINID", domain->GetID());

      std::shared_ptr<DALRecordset> recordset = Application::Instance()->GetDBManager()->OpenRecordset(command);
      if (!recordset)
         return false;

      __int64 accountId = 0;
      while (!recordset->IsEOF())
      {
         String address = recordset->GetStringValue("accountaddress");
         if (HashedLocalPartOf(address) == wanted)
         {
            accountId = recordset->GetInt64Value("accountid");
            break;
         }

         recordset->MoveNext();
      }

      if (accountId == 0)
         return false;

      std::vector<Row> rows;
      if (!List(accountId, rows) || rows.empty())
         return false;

      // Every key the account holds, in the order they were uploaded. A client
      // reading a directory answer imports all of them, which is what makes a key
      // rotation work: the key being retired and the key replacing it are both
      // findable while correspondents catch up.
      for (size_t i = 0; i < rows.size(); i++)
         keyBytes += Base64::Decode(rows[i].key_base64.c_str(), rows[i].key_base64.GetLength());

      return !keyBytes.IsEmpty();
   }

   AnsiString
   OpenPgpKeyStore::HashedLocalPartOf(const String &address)
   {
      String localPart = LocalPartOf(address);
      if (localPart.IsEmpty())
         return "";

      return OpenPgpKey::HashedLocalPart(Utf8(localPart));
   }

   AnsiString
   OpenPgpKeyStore::DirectoryUrl(const String &address, bool advanced)
   {
      AnsiString hash = HashedLocalPartOf(address);
      if (hash.IsEmpty())
         return "";

      AnsiString domain = Utf8(DomainOf(address));
      AnsiString localPart = Utf8(LocalPartOf(address));
      if (domain.IsEmpty())
         return "";

      AnsiString url;
      if (advanced)
         url = "https://openpgpkey." + domain + "/.well-known/openpgpkey/" + domain + "/hu/" + hash;
      else
         url = "https://" + domain + "/.well-known/openpgpkey/hu/" + hash;

      // The ?l= the draft allows. It is not what finds the key - the hash is -
      // but it is what makes a directory answer readable in a log and a URL an
      // administrator can check by eye. Percent-encoded, as the keyserver path
      // is: the local part came from a query string, and a request line is
      // the one place a control character in it would mean something.
      return url + "?l=" + HttpsClient::FormEncode(localPart);
   }

   AnsiString
   OpenPgpKeyStore::AccountJson(std::shared_ptr<const Account> account)
   {
      if (!account)
         return "{}";

      const String address = account->GetAddress();

      std::shared_ptr<const Domain> domain = CacheContainer::Instance()->GetDomain(account->GetDomainID());

      const bool serverEnabled = Configuration::Instance()->GetOpenPGPWebKeyDirectoryEnabled();
      const bool domainEnabled = domain && domain->GetIsActive() && domain->GetOpenPGPPublish();
      const bool accountEnabled = account->GetOpenPGPPublish();

      std::vector<Row> rows;
      List(account->GetID(), rows);

      AnsiString json;
      json.Format("{\"address\":\"%hs\",\"publish\":%hs,\"server_enabled\":%hs,\"domain_enabled\":%hs,\"published\":%hs,"
                  "\"hashed_local_part\":\"%hs\",\"advanced_url\":\"%hs\",\"direct_url\":\"%hs\",\"limit\":%d,\"keys\":[",
                  Escape(Utf8(address)).c_str(),
                  accountEnabled ? "true" : "false",
                  serverEnabled ? "true" : "false",
                  domainEnabled ? "true" : "false",
                  (serverEnabled && domainEnabled && accountEnabled && !rows.empty()) ? "true" : "false",
                  HashedLocalPartOf(address).c_str(),
                  Escape(DirectoryUrl(address, true)).c_str(),
                  Escape(DirectoryUrl(address, false)).c_str(),
                  Configuration::Instance()->GetOpenPGPKeysPerAccount());

      for (size_t i = 0; i < rows.size(); i++)
      {
         if (i > 0)
            json += ",";
         json += RowJson(rows[i], String(), false);
      }

      json += "]}";
      return json;
   }

   AnsiString
   OpenPgpKeyStore::RowJson(const Row &row, const String &address, bool keyIncluded)
   {
      AnsiString json;
      json.Format("{\"id\":%I64d,\"fingerprint\":\"%hs\",\"version\":%d,\"algorithm\":%d,\"bits\":%d,"
                  "\"user_id\":\"%hs\",\"created\":%I64d,\"expires\":%I64d,\"bytes\":%d,"
                  "\"uploaded\":%I64d,\"uploaded_by\":\"%hs\"",
                  row.id,
                  row.fingerprint.c_str(),
                  row.version,
                  row.algorithm,
                  row.bits,
                  Escape(Utf8(row.user_id)).c_str(),
                  row.created,
                  row.expires,
                  row.size,
                  row.uploaded,
                  Escape(Utf8(row.uploaded_by)).c_str());

      if (!address.IsEmpty())
      {
         json += ",\"address\":\"" + Escape(Utf8(address)) + "\"";
         json += ",\"hashed_local_part\":\"" + HashedLocalPartOf(address) + "\"";
         json += ",\"advanced_url\":\"" + Escape(DirectoryUrl(address, true)) + "\"";
         json += ",\"direct_url\":\"" + Escape(DirectoryUrl(address, false)) + "\"";
      }

      if (keyIncluded)
         json += ",\"key\":\"" + row.key_base64 + "\"";

      json += "}";
      return json;
   }
}
